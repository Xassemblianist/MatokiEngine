/* thread/job.c — Sabit kapasiteli kilitsiz MPMC iş kuyruğu implementasyonu.
 * Dmitry Vyukov'un dizi tabanlı sıra numaralı (sequence buffer) MPMC algoritmasını
 * kullanır. Worker'lar boşta kaldığında futex üzerinde 1 ms zaman aşımıyla uyur;
 * böylece yeni iş geldiğinde anında uyanır, iş yokken de JS olay döngüsüne (JIT mesajları)
 * periyodik olarak kontrol verir.
 */
#include "thread/job.h"
#include "thread/atomic.h"
#include "thread/thread.h"
#include "core/log.h"

typedef struct {
    _Alignas(4) u32 seq;
    JobFn           fn;
    void*           data;
    JobCounter*     counter;
} JobSlot;

typedef struct {
    _Alignas(64) JobSlot slots[JOB_QUEUE_CAP];
    _Alignas(64) u32     head;        /* Üreticilerin (enqueue) ilerlettiği indeks */
    _Alignas(64) u32     tail;        /* Tüketicilerin (dequeue) ilerlettiği indeks */
    _Alignas(64) i32     wake_seq;    /* Futex uyandırma sayacı */
    _Alignas(64) u32     initialized; /* 0 = ilklendirilmedi, 1 = ilklendiriliyor, 2 = hazır */
    _Alignas(64) u32     stop_flag;   /* 1 ise worker'lar döngüden çıkar */
} JobQueue;

static JobQueue g_jq;

static void job_ensure_init(void) {
    u32 state = atomic_load_acquire_u32(&g_jq.initialized);
    if (state == 2u) {
        return;
    }
    if (atomic_cas_u32(&g_jq.initialized, 0u, 1u)) {
        for (u32 i = 0; i < JOB_QUEUE_CAP; i++) {
            atomic_store_relaxed_u32(&g_jq.slots[i].seq, i);
            g_jq.slots[i].fn      = NULL;
            g_jq.slots[i].data    = NULL;
            g_jq.slots[i].counter = NULL;
        }
        atomic_store_relaxed_u32(&g_jq.head, 0u);
        atomic_store_relaxed_u32(&g_jq.tail, 0u);
        atomic_store_relaxed_u32((u32*)&g_jq.wake_seq, 0u);
        atomic_store_relaxed_u32(&g_jq.stop_flag, 0u);
        atomic_store_release_u32(&g_jq.initialized, 2u);
    } else {
        while (atomic_load_acquire_u32(&g_jq.initialized) != 2u) {
            atomic_spin_pause();
        }
    }
}

void job_system_init(void) {
    atomic_store_relaxed_u32(&g_jq.initialized, 0u);
    job_ensure_init();
}

void job_counter_init(JobCounter* counter, u32 initial) {
    if (!counter) return;
    atomic_store_release_u32(&counter->pending, initial);
}

static b32 job_enqueue_internal(JobFn fn, void* data, JobCounter* counter) {
    u32 pos = atomic_load_relaxed_u32(&g_jq.head);
    for (;;) {
        JobSlot* slot = &g_jq.slots[pos & JOB_QUEUE_MASK];
        u32 seq = atomic_load_acquire_u32(&slot->seq);
        i32 dif = (i32)seq - (i32)pos;
        if (dif == 0) {
            if (atomic_cas_u32(&g_jq.head, pos, pos + 1u)) {
                slot->fn      = fn;
                slot->data    = data;
                slot->counter = counter;
                atomic_store_release_u32(&slot->seq, pos + 1u);
                return TRUE;
            }
        } else if (dif < 0) {
            /* Kuyruk dolu */
            return FALSE;
        } else {
            pos = atomic_load_relaxed_u32(&g_jq.head);
        }
    }
}

static b32 job_dequeue_internal(JobFn* out_fn, void** out_data, JobCounter** out_counter) {
    u32 pos = atomic_load_relaxed_u32(&g_jq.tail);
    for (;;) {
        JobSlot* slot = &g_jq.slots[pos & JOB_QUEUE_MASK];
        u32 seq = atomic_load_acquire_u32(&slot->seq);
        i32 dif = (i32)seq - (i32)(pos + 1u);
        if (dif == 0) {
            if (atomic_cas_u32(&g_jq.tail, pos, pos + 1u)) {
                *out_fn      = slot->fn;
                *out_data    = slot->data;
                *out_counter = slot->counter;
                atomic_store_release_u32(&slot->seq, pos + JOB_QUEUE_CAP);
                return TRUE;
            }
        } else if (dif < 0) {
            /* Kuyruk boş */
            return FALSE;
        } else {
            pos = atomic_load_relaxed_u32(&g_jq.tail);
        }
    }
}

b32 job_try_run_one(void) {
    job_ensure_init();
    JobFn       fn      = NULL;
    void*       data    = NULL;
    JobCounter* counter = NULL;
    if (!job_dequeue_internal(&fn, &data, &counter)) {
        return FALSE;
    }
    if (fn) {
        fn(data);
    }
    if (counter) {
        atomic_sub_u32(&counter->pending, 1u);
    }
    return TRUE;
}

void job_submit(JobFn fn, void* data, JobCounter* counter) {
    if (!fn) return;
    job_ensure_init();

    if (counter) {
        atomic_add_u32(&counter->pending, 1u);
    }

    /* Kuyruk doluysa bir iş çekip çalıştırarak yer aç */
    while (!job_enqueue_internal(fn, data, counter)) {
        if (!job_try_run_one()) {
            atomic_spin_pause();
        }
    }

    /* Uyuyan bir worker varsa uyandır */
    atomic_add_u32((u32*)&g_jq.wake_seq, 1u);
    futex_wake(&g_jq.wake_seq, 1u);
}

void job_wait(JobCounter* counter) {
    if (!counter) return;
    job_ensure_init();

    while (atomic_load_acquire_u32(&counter->pending) > 0u) {
        if (!job_try_run_one()) {
            atomic_spin_pause();
        }
    }
}

/* ============================================================================
 * job_parallel_for: Atomik iş çalma (work-stealing) tabanlı paralel döngü.
 * Kuyruğu binlerce küçük görevle doldurmak yerine, aktif thread sayısı kadar
 * (en fazla THREAD_MAX_COUNT) görev gönderir; her thread paylaşılan atomik
 * indeks sayacından (next_idx) grain büyüklüğünde dilimler çekerek çalıştırır.
 * ============================================================================ */

typedef struct {
    _Alignas(4) u32  next_idx;
    u32              count;
    u32              grain;
    JobParallelForFn fn;
    void*            ctx;
} ParallelForBatch;

static void parallel_for_worker_task(void* arg) {
    ParallelForBatch* b = (ParallelForBatch*)arg;
    u32 count = b->count;
    u32 grain = b->grain;
    for (;;) {
        u32 start = atomic_add_u32(&b->next_idx, grain);
        if (start >= count) {
            break;
        }
        u32 end = start + grain;
        if (end > count) {
            end = count;
        }
        b->fn(start, end, b->ctx);
    }
}

void job_parallel_for(u32 count, u32 grain, JobParallelForFn fn, void* ctx) {
    if (count == 0 || !fn) return;
    if (grain == 0) grain = 1u;

    u32 total_chunks = (count + grain - 1u) / grain;
    u32 nthreads     = thread_count();

    /* Tek thread modunda veya tek dilimlik işte doğrudan çalıştır */
    if (nthreads <= 1u || total_chunks == 1u) {
        for (u32 start = 0; start < count; start += grain) {
            u32 end = start + grain;
            if (end > count) end = count;
            fn(start, end, ctx);
        }
        return;
    }

    job_ensure_init();

    ParallelForBatch batch;
    atomic_store_relaxed_u32(&batch.next_idx, 0u);
    batch.count = count;
    batch.grain = grain;
    batch.fn    = fn;
    batch.ctx   = ctx;

    JobCounter counter;
    job_counter_init(&counter, 0u);

    /* Worker sayısı kadar (veya toplam dilim - 1 kadar) görevi kuyruğa gönder,
     * ardından ana thread de doğrudan parallel_for_worker_task'a katılsın.
     */
    u32 num_tasks = nthreads - 1u;
    if (num_tasks > total_chunks - 1u) {
        num_tasks = total_chunks - 1u;
    }

    for (u32 i = 0; i < num_tasks; i++) {
        job_submit(parallel_for_worker_task, &batch, &counter);
    }

    /* Uyuyan tüm worker'ları tek seferde uyandır */
    if (num_tasks > 1u) {
        futex_wake(&g_jq.wake_seq, num_tasks);
    }

    /* Çağıran thread (ana thread) kendi payını hemen işler */
    parallel_for_worker_task(&batch);

    /* Worker görevleri hala bitmediyse kuyruktan yardımcı olarak bekle */
    job_wait(&counter);
}

EXPORT(engine_worker_loop) void engine_worker_loop(void) {
    job_ensure_init();

    /* Mevcut tüm işleri tüket */
    while (atomic_load_relaxed_u32(&g_jq.stop_flag) == 0u) {
        if (job_try_run_one()) {
            continue;
        }

        /* Kısa bir spin aşaması (mikrosaniye mertebesinde yeni iş gelirse futex maliyetinden kaçın) */
        b32 found = FALSE;
        for (u32 spin = 0; spin < 256u; spin++) {
            u32 head = atomic_load_relaxed_u32(&g_jq.head);
            u32 tail = atomic_load_relaxed_u32(&g_jq.tail);
            if (head != tail) {
                found = TRUE;
                break;
            }
            atomic_spin_pause();
        }
        if (found) {
            continue;
        }

        /* Kuyruk hala boşsa futex ile en fazla 2 ms uyu, sonra JS olay döngüsüne
         * (JIT çoğaltma mesajlarını alabilmek için) geri dön.
         * Uyandırılırsa (rc == 0) veya wake_seq değiştiyse (rc == 1) hemen döngüye devam et.
         */
        i32 wake_val = (i32)atomic_load_acquire_u32((u32*)&g_jq.wake_seq);
        u32 head     = atomic_load_acquire_u32(&g_jq.head);
        u32 tail     = atomic_load_acquire_u32(&g_jq.tail);
        if (head != tail) {
            continue;
        }

        i32 rc = futex_wait(&g_jq.wake_seq, wake_val, 2000000LL); /* 2 ms = 2,000,000 ns */
        if (rc == 2 || atomic_load_acquire_u32(&g_jq.head) == atomic_load_acquire_u32(&g_jq.tail)) {
            /* Zaman aşımı veya JIT senkronizasyon uyandırması: C'den çıkıp worker.js mesaj kuyruğuna dön */
            break;
        }
    }
}

EXPORT(engine_worker_stop) void engine_worker_stop(void) {
    atomic_store_release_u32(&g_jq.stop_flag, 1u);
    atomic_add_u32((u32*)&g_jq.wake_seq, 1u);
    futex_wake(&g_jq.wake_seq, THREAD_MAX_WORKERS);
}

void job_wake_for_sync(void) {
    atomic_add_u32((u32*)&g_jq.wake_seq, 1u);
    futex_wake(&g_jq.wake_seq, THREAD_MAX_WORKERS);
}
