/* tests/test_job.c — Atomik işlemler, thread yedeği ve iş kuyruğu (MPMC) birim testleri.
 * Host ortamında (tek thread modunda) atomic.h sarmalayıcılarını, iş gönderimi/beklemeyi,
 * kuyruk kapasitesi aşımında work-helping davranışını ve job_parallel_for'u doğrular.
 */
#include "test.h"
#include "thread/atomic.h"
#include "thread/thread.h"
#include "thread/job.h"

TEST(job_atomic_wrappers) {
    _Alignas(4) u32 val = 10u;
    CHECK(atomic_load_u32(&val) == 10u);

    atomic_store_u32(&val, 25u);
    CHECK(atomic_load_acquire_u32(&val) == 25u);

    u32 old = atomic_add_u32(&val, 7u);
    CHECK(old == 25u);
    CHECK(atomic_load_u32(&val) == 32u);

    old = atomic_sub_u32(&val, 12u);
    CHECK(old == 32u);
    CHECK(atomic_load_u32(&val) == 20u);

    CHECK(atomic_cas_u32(&val, 99u, 100u) == FALSE);
    CHECK(atomic_load_u32(&val) == 20u);
    CHECK(atomic_cas_u32(&val, 20u, 42u) == TRUE);
    CHECK(atomic_load_u32(&val) == 42u);

    /* Host modunda thread_id = 0, thread_count = 1 */
    engine_thread_init(0u, 0u);
    engine_set_worker_count(0u);
    CHECK(thread_id() == 0u);
    CHECK(thread_count() == 1u);
}

static void add_job_fn(void* data) {
    u32* p = (u32*)data;
    atomic_add_u32(p, 1u);
}

TEST(job_submit_and_wait_overflow) {
    job_system_init();

    _Alignas(4) u32 sum = 0u;
    JobCounter counter;
    job_counter_init(&counter, 0u);

    /* Kuyruk kapasitesinin (1024) 3 katı kadar iş gönder:
     * tek thread modunda kuyruk dolunca job_submit otomatik olarak
     * iş çekip çalıştırmalı (work-helping) ve kilitlenmemeli.
     */
    const u32 total_jobs = JOB_QUEUE_CAP * 3u + 17u;
    for (u32 i = 0; i < total_jobs; i++) {
        job_submit(add_job_fn, &sum, &counter);
    }

    job_wait(&counter);
    CHECK(counter.pending == 0u);
    CHECK(sum == total_jobs);
}

typedef struct {
    u32* out;
    u32  scale;
} ParTestCtx;

static void par_fill_squares(u32 start, u32 end, void* arg) {
    ParTestCtx* ctx = (ParTestCtx*)arg;
    for (u32 i = start; i < end; i++) {
        ctx->out[i] = i * i * ctx->scale + 3u;
    }
}

TEST(job_parallel_for_single_and_multi_sim) {
    job_system_init();

    enum { N = 4096 };
    static u32 buf1[N];
    static u32 buf2[N];
    for (u32 i = 0; i < N; i++) {
        buf1[i] = 0u;
        buf2[i] = 0u;
    }

    ParTestCtx ctx1 = { buf1, 5u };
    ParTestCtx ctx2 = { buf2, 5u };

    /* 1. Tek thread kısayolu (thread_count() == 1) */
    engine_set_worker_count(0u);
    job_parallel_for(N, 13u, par_fill_squares, &ctx1);

    /* 2. Kuyruk üzerinden çoklu görev dağıtımı yolunu (thread_count() > 1)
     * simüle et: ana thread hem kendi dilimlerini işler hem de job_wait
     * içinde kuyruğa gönderilen görevleri çekip tamamlar.
     */
    engine_set_worker_count(4u);
    job_parallel_for(N, 17u, par_fill_squares, &ctx2);
    engine_set_worker_count(0u);

    for (u32 i = 0; i < N; i++) {
        u32 expected = i * i * 5u + 3u;
        CHECK(buf1[i] == expected);
        CHECK(buf2[i] == expected);
    }
}
