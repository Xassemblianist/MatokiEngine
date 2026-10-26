/* thread/atomic.h — Atomik bellek işlemleri ve futex (wait/notify) sarmalayıcıları.
 * Wasm32 (-matomics) ve host (C11) üzerinde __atomic_* yerleşiklerini kullanır.
 * Tarayıcı ana thread'inde (thread_id() == 0) memory.atomic.wait32 yasak olduğundan
 * futex_wait yalnızca worker thread'lerde donanım/tarayıcı bekleyişine girer.
 */
#ifndef THREAD_ATOMIC_H
#define THREAD_ATOMIC_H

#include "core/types.h"

static inline u32 atomic_load_u32(const u32* addr) {
    return __atomic_load_n(addr, __ATOMIC_SEQ_CST);
}

static inline u32 atomic_load_relaxed_u32(const u32* addr) {
    return __atomic_load_n(addr, __ATOMIC_RELAXED);
}

static inline u32 atomic_load_acquire_u32(const u32* addr) {
    return __atomic_load_n(addr, __ATOMIC_ACQUIRE);
}

static inline void atomic_store_u32(u32* addr, u32 val) {
    __atomic_store_n(addr, val, __ATOMIC_SEQ_CST);
}

static inline void atomic_store_relaxed_u32(u32* addr, u32 val) {
    __atomic_store_n(addr, val, __ATOMIC_RELAXED);
}

static inline void atomic_store_release_u32(u32* addr, u32 val) {
    __atomic_store_n(addr, val, __ATOMIC_RELEASE);
}

/* Atomik karşılaştır-ve-değiştir (CAS).
 * *addr == expected ise *addr = desired yazar ve TRUE döner; aksi halde FALSE döner.
 */
static inline b32 atomic_cas_u32(u32* addr, u32 expected, u32 desired) {
    return __atomic_compare_exchange_n(addr, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
               ? TRUE
               : FALSE;
}

/* Atomik ekleme: işlem ÖNCESİ eski değeri döndürür. */
static inline u32 atomic_add_u32(u32* addr, u32 delta) {
    return __atomic_fetch_add(addr, delta, __ATOMIC_SEQ_CST);
}

/* Atomik çıkarma: işlem ÖNCESİ eski değeri döndürür. */
static inline u32 atomic_sub_u32(u32* addr, u32 delta) {
    return __atomic_fetch_sub(addr, delta, __ATOMIC_SEQ_CST);
}

/* Bellek bariyeri */
static inline void atomic_fence(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Sıkı bekleme (spin) döngülerinde derleyici optimizasyonunu engelleyen ipucu. */
static inline void atomic_spin_pause(void) {
    __asm__ volatile("" ::: "memory");
}

/* Futex bekleme (memory.atomic.wait32):
 * *addr == expected ise timeout_ns nanosaniye boyunca (negatif = süresiz) uyur.
 * Dönüş: 0 = uyandırıldı (ok), 1 = değer eşit değildi (not-equal), 2 = zaman aşımı (timed-out).
 * DİKKAT: Tarayıcı ana thread'inde wait32 çağrılması TypeError fırlatır!
 * Ana thread beklemeleri spin + iş çalma ile yapılmalıdır.
 */
static inline i32 futex_wait(i32* addr, i32 expected, i64 timeout_ns) {
#if defined(__wasm__) && defined(__wasm_atomics__)
    return __builtin_wasm_memory_atomic_wait32(addr, expected, timeout_ns);
#else
    UNUSED(addr);
    UNUSED(expected);
    UNUSED(timeout_ns);
    return 1;
#endif
}

/* Futex uyandırma (memory.atomic.notify):
 * addr üzerinde bekleyen en fazla count adet thread'i uyandırır.
 * Uyandırılan thread sayısını döndürür.
 */
static inline u32 futex_wake(i32* addr, u32 count) {
#if defined(__wasm__) && defined(__wasm_atomics__)
    return __builtin_wasm_memory_atomic_notify(addr, count);
#else
    UNUSED(addr);
    UNUSED(count);
    return 0;
#endif
}

#endif /* THREAD_ATOMIC_H */
