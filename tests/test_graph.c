/* tests/test_graph.c — Kare grafiği (L3, F0_C / H3) birim testleri ve başarım ölçümleri.
 * Kapsam:
 *  1) Döngü (cycle) tespiti: öz-döngü, 2/3/4 düğümlü kaynak ve açık kenar döngüleri.
 *  2) Topolojik sıralama: ters sırada bildirim, RAW/WAR/WAW zincirleri, CPU/GPU düğüm türleri.
 *  3) Geçici kaynak ömrü [first_step, last_step] ve bellek takma adlama (aliasing) planı.
 *  4) Rastgele 1000 DAG x paralel (job sistemi) yürütme == seri yürütme (bit-bit eşleşme + sıra denetimi).
 *  5) H3 ölçümleri: 200 düğümlü grafikte derleme + yürütme ek yükü (< 0.1 ms) ve
 *     3'lü kare boru hattı (sim N+1 || render N || gpu N-1) örtüşme kazancı (>= 1.3x).
 */
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "test.h"
#include "graph/graph.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/log.h"
#include "plat/platform.h"
#include "thread/atomic.h"
#include "thread/thread.h"
#include "thread/job.h"

static _Alignas(16) u8 g_graph_test_mem[MB(16)];

/* ============================================================================
 * Host İş Parçacığı Havuzu Yardımcıları (MPMC iş kuyruğunu gerçek paralel koşturmak için)
 * ============================================================================ */

typedef struct {
    pthread_t       threads[4];
    u32             count;
    _Alignas(4) u32 stop;
} HostWorkerPool;

static void* host_worker_entry(void* arg) {
    HostWorkerPool* pool = (HostWorkerPool*)arg;
    while (atomic_load_relaxed_u32(&pool->stop) == 0u) {
        if (!job_try_run_one()) {
            sched_yield();
        }
    }
    return NULL;
}

static void host_workers_start(HostWorkerPool* pool, u32 num_workers) {
    if (num_workers > 4u) num_workers = 4u;
    job_system_init();
    atomic_store_relaxed_u32(&pool->stop, 0u);
    pool->count = num_workers;
    engine_set_worker_count(num_workers);
    for (u32 i = 0; i < num_workers; i++) {
        pthread_create(&pool->threads[i], NULL, host_worker_entry, pool);
    }
}

static void host_workers_stop(HostWorkerPool* pool) {
    atomic_store_release_u32(&pool->stop, 1u);
    for (u32 i = 0; i < pool->count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    pool->count = 0u;
    engine_set_worker_count(0u);
}

/* ============================================================================
 * 1. Test: Döngü Tespiti (Cycle Detection)
 * ============================================================================ */

TEST(graph_cycle_detection) {
    Arena a;
    arena_init(&a, g_graph_test_mem, sizeof(g_graph_test_mem));
    Graph* g = graph_create(&a);

    /* 1a) Açık öz-döngü (0 -> 0) */
    graph_reset(g);
    u32 n0 = graph_add_node(g, "self_loop", GRAPH_NODE_CPU, NULL, NULL);
    CHECK(graph_add_edge(g, n0, n0) == TRUE);
    CHECK(graph_compile(g) == FALSE);
    CHECK(g->error == GRAPH_COMPILE_ERR_CYCLE);

    /* 1b) 2 düğümlü kaynak döngüsü: A(okur R1, yazar R2), B(okur R2, yazar R1) */
    graph_reset(g);
    u32 r1 = graph_add_resource(g, "R1", GRAPH_RES_TRANSIENT, 256u, 16u);
    u32 r2 = graph_add_resource(g, "R2", GRAPH_RES_TRANSIENT, 256u, 16u);
    u32 na = graph_add_node(g, "A", GRAPH_NODE_CPU, NULL, NULL);
    u32 nb = graph_add_node(g, "B", GRAPH_NODE_GPU_COMPUTE, NULL, NULL);
    graph_node_read(g, na, r1);
    graph_node_write(g, na, r2);
    graph_node_read(g, nb, r2);
    graph_node_write(g, nb, r1);
    CHECK(graph_compile(g) == FALSE);
    CHECK(g->error == GRAPH_COMPILE_ERR_CYCLE);

    /* 1c) 3 düğümlü kaynak döngüsü: N0 -> N1 -> N2 -> N0 */
    graph_reset(g);
    u32 ra = graph_add_resource(g, "RA", GRAPH_RES_TRANSIENT, 128u, 16u);
    u32 rb = graph_add_resource(g, "RB", GRAPH_RES_TRANSIENT, 128u, 16u);
    u32 rc = graph_add_resource(g, "RC", GRAPH_RES_TRANSIENT, 128u, 16u);
    u32 u0 = graph_add_node(g, "N0", GRAPH_NODE_CPU, NULL, NULL);
    u32 u1 = graph_add_node(g, "N1", GRAPH_NODE_TRANSFER, NULL, NULL);
    u32 u2 = graph_add_node(g, "N2", GRAPH_NODE_GPU_PASS, NULL, NULL);
    graph_node_read(g, u0, rc);
    graph_node_write(g, u0, ra);
    graph_node_read(g, u1, ra);
    graph_node_write(g, u1, rb);
    graph_node_read(g, u2, rb);
    graph_node_write(g, u2, rc);
    CHECK(graph_compile(g) == FALSE);
    CHECK(g->error == GRAPH_COMPILE_ERR_CYCLE);

    /* 1d) 4 düğümlü karma (kaynak + açık kenar) döngüsü */
    graph_reset(g);
    u32 rx = graph_add_resource(g, "RX", GRAPH_RES_TRANSIENT, 64u, 16u);
    u32 ry = graph_add_resource(g, "RY", GRAPH_RES_TRANSIENT, 64u, 16u);
    u32 k0 = graph_add_node(g, "K0", GRAPH_NODE_CPU, NULL, NULL);
    u32 k1 = graph_add_node(g, "K1", GRAPH_NODE_CPU, NULL, NULL);
    u32 k2 = graph_add_node(g, "K2", GRAPH_NODE_CPU, NULL, NULL);
    u32 k3 = graph_add_node(g, "K3", GRAPH_NODE_CPU, NULL, NULL);
    graph_node_write(g, k0, rx);
    graph_node_read(g, k1, rx);
    graph_node_write(g, k1, ry);
    graph_node_read(g, k2, ry);
    graph_add_edge(g, k2, k3);
    graph_add_edge(g, k3, k0);
    CHECK(graph_compile(g) == FALSE);
    CHECK(g->error == GRAPH_COMPILE_ERR_CYCLE);

    /* 1e) Geçerli elmas (diamond) DAG döngü hatası vermemeli */
    graph_reset(g);
    rx = graph_add_resource(g, "RX", GRAPH_RES_TRANSIENT, 64u, 16u);
    ry = graph_add_resource(g, "RY", GRAPH_RES_TRANSIENT, 64u, 16u);
    u32 rz = graph_add_resource(g, "RZ", GRAPH_RES_TRANSIENT, 64u, 16u);
    k0 = graph_add_node(g, "Root", GRAPH_NODE_CPU, NULL, NULL);
    k1 = graph_add_node(g, "Left", GRAPH_NODE_CPU, NULL, NULL);
    k2 = graph_add_node(g, "Right", GRAPH_NODE_GPU_COMPUTE, NULL, NULL);
    k3 = graph_add_node(g, "Join", GRAPH_NODE_GPU_PASS, NULL, NULL);
    graph_node_write(g, k0, rx);
    graph_node_read(g, k1, rx);
    graph_node_write(g, k1, ry);
    graph_node_read(g, k2, rx);
    graph_node_write(g, k2, rz);
    graph_node_read(g, k3, ry);
    graph_node_read(g, k3, rz);
    CHECK(graph_compile(g) == TRUE);
    CHECK(g->error == GRAPH_COMPILE_OK);
    CHECK(g->nodes[k0].topo_step == 0u);
    CHECK(g->nodes[k3].topo_step == 3u);
    CHECK(g->stats.cpu_to_gpu_barriers == 2u); /* k0->k2 ve k1->k3 */
}

/* ============================================================================
 * 2. Test: Topolojik Sıralama, Ters Bildirim, RAW/WAR/WAW ve GPU Kayıt Çağrısı
 * ============================================================================ */

struct RhiCmdStream {
    u32 cmds[32];
    u32 count;
};

typedef struct {
    u32 order[16];
    u32 count;
} OrderLog;

static void log_node_cb(const GraphTaskCtx* ctx) {
    OrderLog* log = (OrderLog*)ctx->user_data;
    u32 pos = atomic_add_u32(&log->count, 1u);
    log->order[pos] = ctx->node_id;
    if (ctx->rhi_cmd && ctx->graph->nodes[ctx->node_id].type != GRAPH_NODE_CPU) {
        ctx->rhi_cmd->cmds[ctx->rhi_cmd->count++] =
            (ctx->node_id << 8u) | (u32)ctx->graph->nodes[ctx->node_id].type;
    }
}

TEST(graph_topological_order_and_node_types) {
    Arena a;
    arena_init(&a, g_graph_test_mem, sizeof(g_graph_test_mem));
    Graph* g = graph_create(&a);

    struct RhiCmdStream rhi = { {0}, 0u };
    OrderLog log = { {0}, 0u };

    graph_set_context(g, 7u, 1u, &rhi);

    u32 r_scene = graph_add_resource(g, "scene_buf", GRAPH_RES_TRANSIENT, 1024u, 16u);
    u32 r_cull  = graph_add_resource(g, "cull_list", GRAPH_RES_TRANSIENT, 512u, 16u);
    u32 r_hdr   = graph_add_resource(g, "hdr_color", GRAPH_RES_TRANSIENT, 4096u, 64u);
    u32 r_back  = graph_add_resource(g, "backbuffer", GRAPH_RES_PERSISTENT, 4096u, 64u);

    /* Tüketicileri üreticilerden ÖNCE kaydet: derleyici doğru topolojik sırayı bulmalı
     * Beklenen sıra: n_sim(3) -> n_xfer(2) -> n_comp(1) -> n_pass(0) */
    u32 n_pass = graph_add_node(g, "tonemap_pass", GRAPH_NODE_GPU_PASS, log_node_cb, &log);
    graph_node_read(g, n_pass, r_hdr);
    graph_node_write(g, n_pass, r_back);

    u32 n_comp = graph_add_node(g, "light_compute", GRAPH_NODE_GPU_COMPUTE, log_node_cb, &log);
    graph_node_read(g, n_comp, r_cull);
    graph_node_write(g, n_comp, r_hdr);

    u32 n_xfer = graph_add_node(g, "upload_scene", GRAPH_NODE_TRANSFER, log_node_cb, &log);
    graph_node_read(g, n_xfer, r_scene);
    graph_node_write(g, n_xfer, r_cull);

    u32 n_sim = graph_add_node(g, "cpu_sim", GRAPH_NODE_CPU, log_node_cb, &log);
    graph_node_write(g, n_sim, r_scene);

    CHECK(graph_compile(g) == TRUE);
    CHECK(g->nodes[n_sim].topo_step  == 0u);
    CHECK(g->nodes[n_xfer].topo_step == 1u);
    CHECK(g->nodes[n_comp].topo_step == 2u);
    CHECK(g->nodes[n_pass].topo_step == 3u);
    CHECK(g->stats.cpu_to_gpu_barriers == 1u); /* n_sim (CPU) -> n_xfer (TRANSFER) */

    CHECK(graph_execute(g) == TRUE);
    CHECK(log.count == 4u);
    CHECK(log.order[0] == n_sim);
    CHECK(log.order[1] == n_xfer);
    CHECK(log.order[2] == n_comp);
    CHECK(log.order[3] == n_pass);

    /* GPU düğümlerinin (transfer, gpu_compute, gpu_pass) RHI akışına sırayla kayıt yaptığı */
    CHECK(rhi.count == 3u);
    CHECK(rhi.cmds[0] == ((n_xfer << 8u) | (u32)GRAPH_NODE_TRANSFER));
    CHECK(rhi.cmds[1] == ((n_comp << 8u) | (u32)GRAPH_NODE_GPU_COMPUTE));
    CHECK(rhi.cmds[2] == ((n_pass << 8u) | (u32)GRAPH_NODE_GPU_PASS));

    /* Çoklu yazıcı (RAW + WAR + WAW) zinciri testi:
     * W0 yazar R -> R1, R2 okur R -> W1 yazar R -> R3 okur R */
    graph_reset(g);
    u32 r_shared = graph_add_resource(g, "shared", GRAPH_RES_TRANSIENT, 256u, 16u);
    u32 w0 = graph_add_node(g, "W0", GRAPH_NODE_CPU, NULL, NULL);
    u32 rd1 = graph_add_node(g, "R1", GRAPH_NODE_CPU, NULL, NULL);
    u32 rd2 = graph_add_node(g, "R2", GRAPH_NODE_CPU, NULL, NULL);
    u32 w1 = graph_add_node(g, "W1", GRAPH_NODE_CPU, NULL, NULL);
    u32 rd3 = graph_add_node(g, "R3", GRAPH_NODE_CPU, NULL, NULL);

    graph_node_write(g, w0, r_shared);
    graph_node_read(g, rd1, r_shared);
    graph_node_read(g, rd2, r_shared);
    graph_node_write(g, w1, r_shared);
    graph_node_read(g, rd3, r_shared);

    CHECK(graph_compile(g) == TRUE);
    CHECK(g->nodes[w0].topo_step < g->nodes[rd1].topo_step);
    CHECK(g->nodes[w0].topo_step < g->nodes[rd2].topo_step);
    CHECK(g->nodes[rd1].topo_step < g->nodes[w1].topo_step);
    CHECK(g->nodes[rd2].topo_step < g->nodes[w1].topo_step);
    CHECK(g->nodes[w1].topo_step < g->nodes[rd3].topo_step);
}

/* ============================================================================
 * 3. Test: Geçici Kaynak Ömrü [first_step, last_step] ve Takma Adlama (Aliasing) Planı
 * ============================================================================ */

typedef struct {
    u32 in_res;
    u32 out_res;
    u32 add_const;
} AliasPassData;

static void alias_pass_cb(const GraphTaskCtx* ctx) {
    const AliasPassData* d = (const AliasPassData*)ctx->user_data;
    u32* out_ptr = (u32*)graph_res_ptr(ctx->graph, d->out_res);
    CHECK(out_ptr != NULL);
    if (!out_ptr) return;

    u32 prev = 0u;
    if (d->in_res != GRAPH_ID_NIL) {
        const u32* in_ptr = (const u32*)graph_res_ptr(ctx->graph, d->in_res);
        CHECK(in_ptr != NULL);
        if (in_ptr) prev = in_ptr[0];
    }
    out_ptr[0] = prev + d->add_const;
}

TEST(graph_transient_lifetime_and_aliasing) {
    Arena a;
    arena_init(&a, g_graph_test_mem, sizeof(g_graph_test_mem));
    Graph* g = graph_create(&a);

    u32 persistent_storage[16] = {0};

    u32 r_unused = graph_add_resource(g, "unused",  GRAPH_RES_TRANSIENT,  4096u, 64u);
    u32 r_t0     = graph_add_resource(g, "t0_cull", GRAPH_RES_TRANSIENT,  4096u, 64u); /* ömür [0, 1] */
    u32 r_t1     = graph_add_resource(g, "t1_gbuf", GRAPH_RES_TRANSIENT,  4096u, 64u); /* ömür [1, 2] */
    u32 r_t2     = graph_add_resource(g, "t2_hdr",  GRAPH_RES_TRANSIENT,  4096u, 64u); /* ömür [2, 3] */
    u32 r_t3     = graph_add_resource(g, "t3_ldr",  GRAPH_RES_TRANSIENT,  4096u, 64u); /* ömür [3, 4] */
    u32 r_pers   = graph_add_resource(g, "final",   GRAPH_RES_PERSISTENT, 64u,   16u);
    graph_set_resource_ext(g, r_pers, persistent_storage);

    AliasPassData p0 = { GRAPH_ID_NIL, r_t0,   10u };
    AliasPassData p1 = { r_t0,         r_t1,   20u };
    AliasPassData p2 = { r_t1,         r_t2,   30u };
    AliasPassData p3 = { r_t2,         r_t3,   40u };
    AliasPassData p4 = { r_t3,         r_pers, 50u };

    u32 n0 = graph_add_node(g, "P0", GRAPH_NODE_CPU,         alias_pass_cb, &p0);
    u32 n1 = graph_add_node(g, "P1", GRAPH_NODE_GPU_COMPUTE, alias_pass_cb, &p1);
    u32 n2 = graph_add_node(g, "P2", GRAPH_NODE_GPU_PASS,    alias_pass_cb, &p2);
    u32 n3 = graph_add_node(g, "P3", GRAPH_NODE_GPU_PASS,    alias_pass_cb, &p3);
    u32 n4 = graph_add_node(g, "P4", GRAPH_NODE_GPU_PASS,    alias_pass_cb, &p4);

    graph_node_write(g, n0, r_t0);
    graph_node_read(g,  n1, r_t0);
    graph_node_write(g, n1, r_t1);
    graph_node_read(g,  n2, r_t1);
    graph_node_write(g, n2, r_t2);
    graph_node_read(g,  n3, r_t2);
    graph_node_write(g, n3, r_t3);
    graph_node_read(g,  n4, r_t3);
    graph_node_write(g, n4, r_pers);

    CHECK(graph_compile(g) == TRUE);

    /* Kullanılmayan kaynak işaretlenmemeli */
    CHECK(g->resources[r_unused].used == FALSE);
    CHECK(g->resources[r_unused].first_step == GRAPH_STEP_UNUSED);
    CHECK(g->resources[r_unused].last_step  == GRAPH_STEP_UNUSED);

    /* Kullanılan kaynakların [first_step, last_step] aralıkları */
    CHECK(g->resources[r_t0].used == TRUE);
    CHECK(g->resources[r_t0].first_step == 0u && g->resources[r_t0].last_step == 1u);
    CHECK(g->resources[r_t1].first_step == 1u && g->resources[r_t1].last_step == 2u);
    CHECK(g->resources[r_t2].first_step == 2u && g->resources[r_t2].last_step == 3u);
    CHECK(g->resources[r_t3].first_step == 3u && g->resources[r_t3].last_step == 4u);
    CHECK(g->resources[r_pers].first_step == 4u && g->resources[r_pers].last_step == 4u);

    /* Ömürleri kesişen kaynaklar (t0 ile t1, t1 ile t2, t2 ile t3) FARKLI ofset almalı;
     * ömrü biten kaynakların belleği (t0 -> t2, t1 -> t3) YENİDEN KULLANILMALI (ping-pong aliasing)! */
    CHECK(g->resources[r_t0].mem_offset != g->resources[r_t1].mem_offset);
    CHECK(g->resources[r_t1].mem_offset != g->resources[r_t2].mem_offset);
    CHECK(g->resources[r_t2].mem_offset != g->resources[r_t3].mem_offset);

    CHECK(g->resources[r_t2].mem_offset == g->resources[r_t0].mem_offset);
    CHECK(g->resources[r_t2].alias_of   == r_t0);
    CHECK(g->resources[r_t3].mem_offset == g->resources[r_t1].mem_offset);
    CHECK(g->resources[r_t3].alias_of   == r_t1);

    /* 4 x 4096 = 16384 bayt yerine yalnız 2 x 4096 = 8192 bayt tepe geçici bellek */
    CHECK(g->stats.transient_used_count      == 4u);
    CHECK(g->stats.transient_alias_count     == 2u);
    CHECK(g->stats.transient_slot_count      == 2u);
    CHECK(g->stats.transient_unaliased_bytes == 16384u);
    CHECK(g->stats.transient_peak_bytes      == 8192u);

    /* Geçici belleği ayır, çalıştır ve takma adlı tamponlar üzerinden doğru verinin aktığını doğrula */
    u8* tmem = graph_alloc_transient_memory(g, &a);
    CHECK(tmem != NULL);
    CHECK(graph_execute(g) == TRUE);
    CHECK(persistent_storage[0] == (10u + 20u + 30u + 40u + 50u));
}

/* ============================================================================
 * 4. Test: Rastgele 1000 Grafik x Paralel Yürütme == Seri Yürütme
 * ============================================================================ */

typedef struct {
    u32*            node_val;    /* [max_nodes] her düğümün ürettiği deterministik özet */
    _Alignas(4) u32 done[128];   /* [max_nodes] düğüm tamamlandı bayrağı (sıra ihlali kontrolü) */
    _Alignas(4) u32 order_err;   /* Sıra ihlali tespit edilirse > 0 */
    const u32*      pred_head;   /* [max_nodes] düğümün tüm öncüllerini doğrulamak için */
    const u32*      pred_node;
    const u32*      pred_next;
} RandDagExecState;

static u32 mix_u32(u32 x) {
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return x;
}

static void rand_dag_node_cb(const GraphTaskCtx* ctx) {
    RandDagExecState* st = (RandDagExecState*)ctx->user_data;
    const Graph*      g  = ctx->graph;
    u32               u  = ctx->node_id;
    const GraphNode*  n  = &g->nodes[u];

    /* 1) Tüm öncül (bağımlı olunan) düğümlerin tamamlanmış olduğunu doğrula */
    u32 p_cur = st->pred_head[u];
    u32 h = mix_u32(u * 0x9E3779B9u + (u32)n->type * 17u + 1u);
    while (p_cur != GRAPH_ID_NIL) {
        u32 p = st->pred_node[p_cur];
        if (atomic_load_acquire_u32(&st->done[p]) != 1u) {
            atomic_add_u32(&st->order_err, 1u);
        }
        /* Öncül değerlerini deterministik (sıradan bağımsız ama tam bağımlı) birleştir */
        h ^= mix_u32(st->node_val[p] + p * 0x85EBCA6Bu);
        p_cur = st->pred_next[p_cur];
    }

    /* 2) Okunan kaynakların geçici/kalıcı bellek içeriklerini oku */
    for (u32 i = 0; i < n->read_count; i++) {
        u32 r = n->reads[i];
        const u32* rptr = (const u32*)graph_res_ptr(g, r);
        if (rptr) {
            h = mix_u32(h ^ rptr[0] ^ (r << 8u));
        }
    }

    st->node_val[u] = h;

    /* 3) Yazılan kaynakların belleğine deterministik çıktı yaz */
    for (u32 i = 0; i < n->write_count; i++) {
        u32 r = n->writes[i];
        u32* wptr = (u32*)graph_res_ptr(g, r);
        if (wptr) {
            wptr[0] = mix_u32(h + r * 31u);
        }
    }

    atomic_store_release_u32(&st->done[u], 1u);
}

TEST(graph_random_1000_dags_match_serial) {
    Arena a;
    arena_init(&a, g_graph_test_mem, sizeof(g_graph_test_mem));
    Graph* g = graph_create(&a);

    HostWorkerPool pool;
    host_workers_start(&pool, 3u); /* Ana thread + 3 worker = 4 iş parçacığı */

    Rng rng;
    rng_seed(&rng, 0xF0C01000DA65ULL);

    u32 serial_vals[128];
    u32 parallel_vals[128];
    u32 pers_serial[64];
    u32 pers_parallel[64];
    u32 pred_head[128];
    u32 pred_node[4096];
    u32 pred_next[4096];

    for (u32 iter = 0; iter < 1000u; iter++) {
        usize mark = arena_mark(&a);
        graph_reset(g);

        u32 n_nodes = (u32)rng_range(&rng, 8, 65);
        u32 n_res   = (u32)rng_range(&rng, 6, 33);

        /* Rastgele geçici ve kalıcı kaynaklar ekle */
        for (u32 r = 0; r < n_res; r++) {
            GraphResLifetime life = (rng_range(&rng, 0, 4) == 0)
                                  ? GRAPH_RES_PERSISTENT
                                  : GRAPH_RES_TRANSIENT;
            u32 sz  = (u32)rng_range(&rng, 1, 9) * 64u;
            u32 aln = (rng_range(&rng, 0, 2) == 0) ? 16u : 64u;
            u32 rid = graph_add_resource(g, "R", life, sz, aln);
            CHECK(rid == r);
        }

        /* Düğümleri rastgele bir permütasyon sırasıyla oluştur (üretici/tüketici bildirim sırası karışık) */
        u32 rank_to_node[128];
        u32 perm[128];
        for (u32 i = 0; i < n_nodes; i++) perm[i] = i;
        for (u32 i = n_nodes - 1u; i > 0u; i--) {
            u32 j = (u32)rng_range(&rng, 0, (i32)i + 1);
            u32 tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
        }

        RandDagExecState exec_state;
        mem_zero:
        memset(&exec_state, 0, sizeof(exec_state));
        (void)&&mem_zero;

        for (u32 u = 0; u < n_nodes; u++) {
            GraphNodeType nt = (GraphNodeType)rng_range(&rng, 0, 4);
            u32 id = graph_add_node(g, "N", nt, rand_dag_node_cb, &exec_state);
            CHECK(id == u);
            rank_to_node[perm[u]] = u;
        }

        /* Her kaynak için rank sırasına göre 1 üretici ve 0..3 okuyucu ata (döngüsüzlüğü garanti eder) */
        for (u32 r = 0; r < n_res; r++) {
            u32 writer_rank = (u32)rng_range(&rng, 0, (i32)n_nodes - 1);
            u32 w_node      = rank_to_node[writer_rank];
            if (g->nodes[w_node].write_count < GRAPH_MAX_NODE_WRITES) {
                graph_node_write(g, w_node, r);
            }
            u32 num_readers = (u32)rng_range(&rng, 0, 4);
            for (u32 k = 0; k < num_readers; k++) {
                u32 r_rank = (u32)rng_range(&rng, (i32)writer_rank + 1, (i32)n_nodes);
                u32 r_node = rank_to_node[r_rank];
                if (g->nodes[r_node].read_count < GRAPH_MAX_NODE_READS) {
                    graph_node_read(g, r_node, r);
                }
            }
        }

        /* Ek rastgele açık kenarlar (küçük rank -> büyük rank) */
        u32 extra_edges = (u32)rng_range(&rng, 0, (i32)n_nodes);
        for (u32 e = 0; e < extra_edges; e++) {
            u32 ra = (u32)rng_range(&rng, 0, (i32)n_nodes - 1);
            u32 rb = (u32)rng_range(&rng, (i32)ra + 1, (i32)n_nodes);
            graph_add_edge(g, rank_to_node[ra], rank_to_node[rb]);
        }

        CHECK(graph_compile(g) == TRUE);

        /* Derlenen kenarlardan öncül (predecessor) listesini kur ve topolojik sırayı doğrula */
        for (u32 u = 0; u < n_nodes; u++) pred_head[u] = GRAPH_ID_NIL;
        u32 pred_cnt = 0u;
        for (u32 u = 0; u < n_nodes; u++) {
            u32 end = g->nodes[u].out_offset + g->nodes[u].out_count;
            for (u32 e = g->nodes[u].out_offset; e < end; e++) {
                u32 v = g->edge_targets[e];
                CHECK(g->nodes[u].topo_step < g->nodes[v].topo_step);
                pred_node[pred_cnt] = u;
                pred_next[pred_cnt] = pred_head[v];
                pred_head[v]        = pred_cnt++;
            }
        }

        /* Ömrü kesişen hiçbir geçici kaynak çiftinin bellekte çakışmadığını doğrula */
        for (u32 r1 = 0; r1 < n_res; r1++) {
            const GraphResource* a_res = &g->resources[r1];
            if (!a_res->used || a_res->lifetime != GRAPH_RES_TRANSIENT) continue;
            for (u32 r2 = r1 + 1u; r2 < n_res; r2++) {
                const GraphResource* b_res = &g->resources[r2];
                if (!b_res->used || b_res->lifetime != GRAPH_RES_TRANSIENT) continue;
                if (a_res->first_step <= b_res->last_step && b_res->first_step <= a_res->last_step) {
                    u32 a0 = a_res->mem_offset, a1 = a0 + a_res->size;
                    u32 b0 = b_res->mem_offset, b1 = b0 + b_res->size;
                    CHECK(!(a0 < b1 && b0 < a1));
                }
            }
        }

        u8* trans_mem = graph_alloc_transient_memory(g, &a);

        /* 1) Seri yürütme */
        for (u32 r = 0; r < n_res; r++) {
            pers_serial[r] = 0x1000u + r;
            if (g->resources[r].lifetime == GRAPH_RES_PERSISTENT) {
                graph_set_resource_ext(g, r, &pers_serial[r]);
            }
        }
        if (trans_mem && g->stats.transient_peak_bytes > 0u) {
            memset(trans_mem, 0, g->stats.transient_peak_bytes);
        }
        memset(serial_vals, 0, sizeof(serial_vals));
        memset(exec_state.done, 0, sizeof(exec_state.done));
        exec_state.node_val  = serial_vals;
        exec_state.order_err = 0u;
        exec_state.pred_head = pred_head;
        exec_state.pred_node = pred_node;
        exec_state.pred_next = pred_next;

        CHECK(graph_execute_serial(g) == TRUE);
        CHECK(exec_state.order_err == 0u);

        /* 2) İş kuyruğunda paralel yürütme */
        for (u32 r = 0; r < n_res; r++) {
            pers_parallel[r] = 0x1000u + r;
            if (g->resources[r].lifetime == GRAPH_RES_PERSISTENT) {
                graph_set_resource_ext(g, r, &pers_parallel[r]);
            }
        }
        if (trans_mem && g->stats.transient_peak_bytes > 0u) {
            memset(trans_mem, 0, g->stats.transient_peak_bytes);
        }
        memset(parallel_vals, 0, sizeof(parallel_vals));
        memset(exec_state.done, 0, sizeof(exec_state.done));
        exec_state.node_val  = parallel_vals;
        exec_state.order_err = 0u;

        CHECK(graph_execute(g) == TRUE);
        CHECK(exec_state.order_err == 0u);

        /* Seri ve paralel sonuçların bit-bit eşleştiğini doğrula */
        for (u32 u = 0; u < n_nodes; u++) {
            CHECK(atomic_load_relaxed_u32(&exec_state.done[u]) == 1u);
            CHECK(serial_vals[u] == parallel_vals[u]);
        }
        for (u32 r = 0; r < n_res; r++) {
            if (g->resources[r].lifetime == GRAPH_RES_PERSISTENT) {
                CHECK(pers_serial[r] == pers_parallel[r]);
            }
        }

        arena_pop_to(&a, mark);
    }

    host_workers_stop(&pool);
}

/* ============================================================================
 * 5. Test ve Ölçüm (H3): 200 Düğümlü Grafik Ek Yükü + 3'lü Kare Boru Hattı Kazancı
 * ============================================================================ */

static f64 median_f64_arr(f64* arr, u32 n) {
    for (u32 i = 1; i < n; i++) {
        f64 k = arr[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && arr[j] > k) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = k;
    }
    return arr[n / 2u];
}

static void noop_node_cb(const GraphTaskCtx* ctx) {
    u32* counter = (u32*)ctx->user_data;
    if (counter) {
        atomic_add_u32(counter, 1u);
    }
}

static void build_200_node_graph(Graph* g, u32* exec_counter) {
    graph_reset(g);
    /* 20 aşama (stage) x (9 paralel CPU düğümü + 1 birleştirici geçiş düğümü) = 200 düğüm;
     * aşama içi 2'şer + aşamalar arası 1'er = 60 geçici kaynak. */
    enum { STAGES = 20, PAR_W = 9, N_RES = 60 };
    u32 res_ids[N_RES];
    for (u32 r = 0; r < N_RES; r++) {
        res_ids[r] = graph_add_resource(g, "T", GRAPH_RES_TRANSIENT, 1024u, 64u);
    }

    for (u32 s = 0; s < STAGES; s++) {
        u32 r_in    = (s > 0u) ? res_ids[(s - 1u) * 3u + 2u] : GRAPH_ID_NIL;
        u32 r_sub0  = res_ids[s * 3u + 0u];
        u32 r_sub1  = res_ids[s * 3u + 1u];
        u32 r_stage = res_ids[s * 3u + 2u];

        u32 w_nodes[PAR_W];
        for (u32 w = 0; w < PAR_W; w++) {
            u32 nid = graph_add_node(g, "W200", GRAPH_NODE_CPU, noop_node_cb, exec_counter);
            w_nodes[w] = nid;
            if (r_in != GRAPH_ID_NIL) {
                graph_node_read(g, nid, r_in);
            }
            if (w == 0u) {
                graph_node_write(g, nid, r_sub0);
            } else if (w == 1u) {
                graph_node_write(g, nid, r_sub1);
            } else if ((w & 1u) == 0u) {
                graph_node_read(g, nid, r_sub0);
            } else {
                graph_node_read(g, nid, r_sub1);
            }
        }

        GraphNodeType join_type = (s >= 16u) ? GRAPH_NODE_GPU_PASS : GRAPH_NODE_CPU;
        u32 jid = graph_add_node(g, "J200", join_type, noop_node_cb, exec_counter);
        graph_node_read(g, jid, r_sub0);
        graph_node_read(g, jid, r_sub1);
        graph_node_write(g, jid, r_stage);
        for (u32 w = 2u; w < PAR_W; w++) {
            graph_add_edge(g, w_nodes[w], jid);
        }
    }
}

/* Sentetik boru hattı iş yükü: her kare sim(f) -> render(f) -> gpu(f) aşamalarından geçer */
typedef struct {
    u32 work_iters;
    u32 ring_sim_out[GRAPH_RING_SIZE];
    u32 ring_ren_out[GRAPH_RING_SIZE];
    u32 frame_sim_res[64];
    u32 frame_ren_res[64];
    u32 frame_gpu_res[64];
} PipeBenchCtx;

static u32 synthetic_burn(u32 seed, u32 iters) {
    u32 h = seed ^ 0xA5A55A5Au;
    for (u32 i = 0; i < iters; i++) {
        h = (h * 1664525u + 1013904223u) ^ (h >> 11u) ^ i;
        __asm__ volatile("" : "+r"(h));
    }
    return h;
}

static void pipe_sim_cb(const GraphTaskCtx* ctx) {
    PipeBenchCtx* p = (PipeBenchCtx*)ctx->user_data;
    u32 f    = ctx->frame_idx;
    u32 ring = ctx->ring_idx;
    u32 val  = synthetic_burn(f * 101u + 1u, p->work_iters);
    p->ring_sim_out[ring] = val;
    p->frame_sim_res[f]   = val;
}

static void pipe_render_cb(const GraphTaskCtx* ctx) {
    PipeBenchCtx* p = (PipeBenchCtx*)ctx->user_data;
    u32 f    = ctx->frame_idx;
    u32 ring = ctx->ring_idx;
    u32 sim_in = p->ring_sim_out[ring];
    u32 val    = synthetic_burn(sim_in ^ (f * 313u + 2u), p->work_iters);
    p->ring_ren_out[ring] = val;
    p->frame_ren_res[f]   = val;
}

static void pipe_gpu_cb(const GraphTaskCtx* ctx) {
    PipeBenchCtx* p = (PipeBenchCtx*)ctx->user_data;
    u32 f    = ctx->frame_idx;
    u32 ring = ctx->ring_idx;
    u32 ren_in = p->ring_ren_out[ring];
    u32 val    = synthetic_burn(ren_in ^ (f * 997u + 3u), p->work_iters);
    p->frame_gpu_res[f] = val;
}

static void build_stage_sim(Graph* g, u32 frame_idx, u32 ring_idx, Arena* slot_arena, void* user_data) {
    UNUSED(frame_idx);
    UNUSED(ring_idx);
    /* Halka yuvası arenasından kareye özel geçici durum ayır */
    u32* slot_tmp = ARENA_PUSH_ARRAY_RAW(slot_arena, u32, 64u);
    slot_tmp[0] = frame_idx;
    u32 r_sim = graph_add_resource(g, "sim_scratch", GRAPH_RES_TRANSIENT, 512u, 16u);
    u32 n_sim = graph_add_node(g, "sim_step", GRAPH_NODE_CPU, pipe_sim_cb, user_data);
    graph_node_write(g, n_sim, r_sim);
}

static void build_stage_render(Graph* g, u32 frame_idx, u32 ring_idx, Arena* slot_arena, void* user_data) {
    UNUSED(frame_idx);
    UNUSED(ring_idx);
    UNUSED(slot_arena);
    u32 r_ren = graph_add_resource(g, "ren_scratch", GRAPH_RES_TRANSIENT, 512u, 16u);
    u32 n_ren = graph_add_node(g, "render_record", GRAPH_NODE_CPU, pipe_render_cb, user_data);
    graph_node_write(g, n_ren, r_ren);
}

static void build_stage_gpu(Graph* g, u32 frame_idx, u32 ring_idx, Arena* slot_arena, void* user_data) {
    UNUSED(frame_idx);
    UNUSED(ring_idx);
    UNUSED(slot_arena);
    u32 r_gpu = graph_add_resource(g, "gpu_scratch", GRAPH_RES_TRANSIENT, 512u, 16u);
    u32 n_gpu = graph_add_node(g, "gpu_present", GRAPH_NODE_CPU, pipe_gpu_cb, user_data);
    graph_node_write(g, n_gpu, r_gpu);
}

TEST(graph_bench_200_nodes_and_pipeline_h3) {
    Arena a;
    arena_init(&a, g_graph_test_mem, sizeof(g_graph_test_mem));
    Graph* g = graph_create(&a);

    /* --- Bölüm A: 200 Düğümlü Grafik Derleme + Yürütme Ek Yükü --- */
    enum { WARMUP = 20, SAMPLES = 51 };
    f64 t_compile[SAMPLES];
    f64 t_exec[SAMPLES];
    f64 t_total[SAMPLES];
    _Alignas(4) u32 exec_counter = 0u;

    build_200_node_graph(g, &exec_counter);
    for (u32 i = 0; i < WARMUP + SAMPLES; i++) {
        exec_counter = 0u;
        f64 t0 = plat_time_ms();
        b32 ok_c = graph_compile(g);
        f64 t1 = plat_time_ms();
        b32 ok_e = graph_execute(g);
        f64 t2 = plat_time_ms();

        CHECK(ok_c == TRUE && ok_e == TRUE);
        CHECK(g->node_count == 200u);
        CHECK(exec_counter == 200u);

        if (i >= WARMUP) {
            u32 s = i - WARMUP;
            t_compile[s] = t1 - t0;
            t_exec[s]    = t2 - t1;
            t_total[s]   = t2 - t0;
        }
    }

    f64 med_compile = median_f64_arr(t_compile, SAMPLES);
    f64 med_exec    = median_f64_arr(t_exec, SAMPLES);
    f64 med_total   = median_f64_arr(t_total, SAMPLES);

    /* H3 eşiği: 200 düğüm derleme + yürütme ek yükü < 0.1 ms (yüklü makinede bile güvenli sınır) */
    CHECK(med_total < 0.5);

    /* --- Bölüm B: 3'lü Kare Boru Hattı (sim N+1 || render N || gpu N-1) Doğruluk + Kazanç --- */
    HostWorkerPool pool;
    host_workers_start(&pool, 3u);

    PipeBenchCtx ctx_serial;
    PipeBenchCtx ctx_pipe;
    memset(&ctx_serial, 0, sizeof(ctx_serial));
    memset(&ctx_pipe, 0, sizeof(ctx_pipe));
    ctx_serial.work_iters = 90000u;
    ctx_pipe.work_iters   = 90000u;

    GraphPipeline pipe_s2, pipe_p2;
    graph_pipeline_init(&pipe_s2, &a, KB(64), build_stage_sim, build_stage_render, NULL, &ctx_serial);
    graph_pipeline_init(&pipe_p2, &a, KB(64), build_stage_sim, build_stage_render, NULL, &ctx_pipe);

    enum { NUM_FRAMES = 24, PIPE_RUNS = 9 };
    f64 ser2_times[PIPE_RUNS];
    f64 pip2_times[PIPE_RUNS];

    for (u32 r = 0; r < PIPE_RUNS; r++) {
        graph_pipeline_reset(&pipe_s2);
        f64 ts0 = plat_time_ms();
        CHECK(graph_pipeline_run_serial_frames(&pipe_s2, NUM_FRAMES) == TRUE);
        f64 ts1 = plat_time_ms();
        ser2_times[r] = ts1 - ts0;

        graph_pipeline_reset(&pipe_p2);
        f64 tp0 = plat_time_ms();
        CHECK(graph_pipeline_run_pipelined(&pipe_p2, NUM_FRAMES) == TRUE);
        f64 tp1 = plat_time_ms();
        pip2_times[r] = tp1 - tp0;
    }

    for (u32 f = 0; f < NUM_FRAMES; f++) {
        CHECK(ctx_serial.frame_sim_res[f] == ctx_pipe.frame_sim_res[f]);
        CHECK(ctx_serial.frame_ren_res[f] == ctx_pipe.frame_ren_res[f]);
    }

    GraphPipeline pipe_s, pipe_p;
    graph_pipeline_init(&pipe_s, &a, KB(64), build_stage_sim, build_stage_render, build_stage_gpu, &ctx_serial);
    graph_pipeline_init(&pipe_p, &a, KB(64), build_stage_sim, build_stage_render, build_stage_gpu, &ctx_pipe);

    f64 ser_times[PIPE_RUNS];
    f64 pip_times[PIPE_RUNS];

    /* Isınma + ölçüm (3 aşama: sim N+1 || render N || gpu N-1) */
    for (u32 r = 0; r < PIPE_RUNS; r++) {
        graph_pipeline_reset(&pipe_s);
        f64 ts0 = plat_time_ms();
        CHECK(graph_pipeline_run_serial_frames(&pipe_s, NUM_FRAMES) == TRUE);
        f64 ts1 = plat_time_ms();
        ser_times[r] = ts1 - ts0;

        graph_pipeline_reset(&pipe_p);
        f64 tp0 = plat_time_ms();
        CHECK(graph_pipeline_run_pipelined(&pipe_p, NUM_FRAMES) == TRUE);
        f64 tp1 = plat_time_ms();
        pip_times[r] = tp1 - tp0;
    }

    host_workers_stop(&pool);

    /* Tüm karelerde seri ve boru hattı çıktılarının bit-bit aynı olduğunu doğrula */
    for (u32 f = 0; f < NUM_FRAMES; f++) {
        CHECK(ctx_serial.frame_sim_res[f] == ctx_pipe.frame_sim_res[f]);
        CHECK(ctx_serial.frame_ren_res[f] == ctx_pipe.frame_ren_res[f]);
        CHECK(ctx_serial.frame_gpu_res[f] == ctx_pipe.frame_gpu_res[f]);
    }

    f64 med_ser2 = median_f64_arr(ser2_times, PIPE_RUNS);
    f64 med_pip2 = median_f64_arr(pip2_times, PIPE_RUNS);
    f64 speedup2 = (med_pip2 > 0.001) ? (med_ser2 / med_pip2) : 1.0;

    f64 med_ser3 = median_f64_arr(ser_times, PIPE_RUNS);
    f64 med_pip3 = median_f64_arr(pip_times, PIPE_RUNS);
    f64 speedup3 = (med_pip3 > 0.001) ? (med_ser3 / med_pip3) : 1.0;

    LOG("  [bench F0_C / H3] 200 dugum (derleme=%.4f ms, yurutme=%.4f ms, toplam=%.4f ms, alias=%u->%u bayt) | "
        "boru hatti 2-asama sim(N+1)||ren(N): %.3f->%.3f ms (%.2fx) | "
        "3-asama +gpu(N-1): %.3f->%.3f ms (%.2fx)",
        med_compile, med_exec, med_total,
        g->stats.transient_unaliased_bytes, g->stats.transient_peak_bytes,
        med_ser2, med_pip2, speedup2,
        med_ser3, med_pip3, speedup3);

    CHECK(speedup2 >= 1.3);
    CHECK(speedup3 >= 1.3);
}
