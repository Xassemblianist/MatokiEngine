/* phys2/par.h — fizikte paralel döngü: thread/job.h'nin job_parallel_for'u üzerine ince sarmalayıcı.
 * threads == 1 → her zaman seri (ölçüm/test); 0 → iş sistemindeki tüm thread'ler. Dilim sınırları
 * sonucu etkilememelidir: çağıranlar iş parçalarını bağımsız (ya da aynı sırada) hesaplar → seri ve
 * paralel yol bit-bit aynı çıktı verir. İş yoksa (tek thread) job_parallel_for dilimleri sırayla çalıştırır.
 * SamurayBalgami/Matoki'de iş sistemi yoktu; bu dosya bu portun eklentisidir.
 */
#ifndef PHYS2_PAR_H
#define PHYS2_PAR_H
#include "core/types.h"
#include "thread/thread.h"
#include "thread/job.h"

/* Bundan az iş varsa dağıtımın maliyeti (kuyruk + uyandırma) kazancı aşar; seri çalıştır. */
#define PHYS2_PAR_MIN_PAIRS 128u     /* dar faz: manifold hesabı yapılacak çift */

typedef void (*Phys2ParFn)(u32 start, u32 end, void* ctx);

/* [0, count) aralığını grain'lik dilimlerle fn(start, end, ctx)'e böler. Senkron. */
static inline void phys2_par_for(u32 threads, u32 count, u32 grain, Phys2ParFn fn, void* ctx) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    if (threads == 1 || count <= grain || thread_count() <= 1u) {
        fn(0, count, ctx);
        return;
    }
    job_parallel_for(count, grain, fn, ctx);
}

#endif
