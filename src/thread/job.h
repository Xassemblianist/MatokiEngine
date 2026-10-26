/* thread/job.h — Çok üreticili, çok tüketicili (MPMC) kilitsiz iş kuyruğu.
 * Sabit kapasiteli (Dmitry Vyukov bounded MPMC queue) halka tampon üzerinden
 * dinamik iş dağıtımı, paralel döngü (job_parallel_for) ve sayaç bekleme
 * (job_wait: ana thread de iş alarak bekler) işlemlerini sağlar.
 */
#ifndef THREAD_JOB_H
#define THREAD_JOB_H

#include "core/types.h"
#include "plat/platform.h"

/* Kuyruk kapasitesi (2'nin kuvveti olmalı) */
#define JOB_QUEUE_CAP  1024u
#define JOB_QUEUE_MASK (JOB_QUEUE_CAP - 1u)

/* Tekil iş fonksiyonu imzası */
typedef void (*JobFn)(void* data);

/* Paralel döngü dilimi fonksiyonu imzası: [start, end) aralığını işler */
typedef void (*JobParallelForFn)(u32 start, u32 end, void* ctx);

/* İş tamamlanma sayacı: pending == 0 olduğunda ilişkili işler bitmiştir */
typedef struct {
    _Alignas(4) u32 pending;
} JobCounter;

/* İş sistemini sıfırlar / başlatır (isteğe bağlı; statik olarak da sıfır başlar). */
void job_system_init(void);

/* Sayacı verilen başlangıç değeriyle (genellikle 0) ilklendirir. */
void job_counter_init(JobCounter* counter, u32 initial);

/* Kuyruğa tek bir iş ekler. counter != NULL ise gönderimde artırılır, iş bitince azaltılır.
 * Kuyruk doluysa çağıran thread kendi kuyruktan bir iş çalıştırarak yer açar.
 */
void job_submit(JobFn fn, void* data, JobCounter* counter);

/* Sayacın 0 olmasını bekler. Beklerken çağıran thread (ana thread dahil) kuyruktan
 * iş çekip çalıştırarak üretime katkıda bulunur (work-helping), kilitlenmeyi önler.
 */
void job_wait(JobCounter* counter);

/* [0, count) aralığını grain büyüklüğünde parçalara bölerek paralel çalıştırır
 * ve tüm parçalar tamamlanana kadar bekler (senkron).
 */
void job_parallel_for(u32 count, u32 grain, JobParallelForFn fn, void* ctx);

/* Kuyruktan tek bir iş çekip çalıştırmayı dener. İş çalıştırdıysa TRUE döner. */
b32  job_try_run_one(void);

/* Worker thread döngüsü: kuyruktan iş çeker, iş yoksa kısa süreli (1 ms) futex_wait
 * ile uyur ve JS mesaj kuyruğunun (JIT çoğaltma vb.) boşaltılabilmesi için geri döner.
 * Worker JS tarafı bu fonksiyonu aktif olduğu sürece döngüde çağırır.
 */
EXPORT(engine_worker_loop) void engine_worker_loop(void);

/* Worker döngüsünü sonlandırma bayrağını ayarlar (test/kapatma için). */
EXPORT(engine_worker_stop) void engine_worker_stop(void);

/* Uyuyan tüm worker'ları JS mesaj kuyruğunu (JIT çoğaltma vb.) hemen işlemeleri için uyandırır. */
void job_wake_for_sync(void);

#endif /* THREAD_JOB_H */
