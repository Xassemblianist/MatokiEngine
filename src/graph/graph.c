/* graph/graph.c — Kare grafiği (L3) derleyicisi, geçici kaynak takma adlayıcısı ve yürütücüsü.
 * Kaynak okuma/yazma bildirimlerinden RAW/WAR/WAW bağımlılık kenarlarını çıkarır, Kahn algoritması
 * ile döngü denetimi ve topolojik sıralama yapar, geçici kaynaklar için çakışmasız bellek ofsetleri
 * (aliasing) hesaplar ve düğümleri MPMC iş kuyruğunda bağımlılık sayaçlarıyla bariyersiz çalıştırır.
 */
#include "graph/graph.h"
#include "thread/atomic.h"
#include "thread/thread.h"
#include "thread/job.h"

typedef struct {
    Graph*      g;
    JobCounter* counter;
    u32         node_id;
} GraphJobItem;

static u32 norm_align(u32 align) {
    if (align <= GRAPH_DEFAULT_ALIGN) {
        return GRAPH_DEFAULT_ALIGN;
    }
    u32 p = GRAPH_DEFAULT_ALIGN;
    while (p < align && p < (1u << 30)) {
        p <<= 1u;
    }
    return p;
}

static void mem_zero_bytes(void* ptr, usize len) {
    u8* p = (u8*)ptr;
    for (usize i = 0; i < len; i++) {
        p[i] = 0;
    }
}

void graph_init(Graph* g, Arena* a, u32 max_nodes, u32 max_resources) {
    if (!g || !a) return;
    if (max_nodes == 0u) max_nodes = GRAPH_MAX_NODES;
    if (max_resources == 0u) max_resources = GRAPH_MAX_RESOURCES;

    u32 edge_cap = max_nodes * (GRAPH_MAX_NODE_READS + GRAPH_MAX_NODE_WRITES + 8u);
    if (edge_cap < 256u) edge_cap = 256u;

    mem_zero_bytes(g, sizeof(*g));
    g->arena         = a;
    g->max_nodes     = max_nodes;
    g->max_resources = max_resources;
    g->raw_edge_cap  = edge_cap;
    g->edge_cap      = edge_cap;

    g->nodes         = ARENA_PUSH_ARRAY(a, GraphNode, max_nodes);
    g->resources     = ARENA_PUSH_ARRAY(a, GraphResource, max_resources);
    g->raw_edge_from = ARENA_PUSH_ARRAY_RAW(a, u32, edge_cap);
    g->raw_edge_to   = ARENA_PUSH_ARRAY_RAW(a, u32, edge_cap);
    g->topo_order    = ARENA_PUSH_ARRAY_RAW(a, u32, max_nodes);
    g->edge_targets  = ARENA_PUSH_ARRAY_RAW(a, u32, edge_cap);
    g->rem_deps      = ARENA_PUSH_ARRAY(a, u32, max_nodes);
    g->job_items     = ARENA_PUSH_ARRAY(a, GraphJobItem, max_nodes);

    graph_reset(g);
}

Graph* graph_create(Arena* a) {
    if (!a) return NULL;
    Graph* g = ARENA_PUSH_STRUCT(a, Graph);
    graph_init(g, a, GRAPH_MAX_NODES, GRAPH_MAX_RESOURCES);
    return g;
}

void graph_reset(Graph* g) {
    if (!g) return;
    g->node_count      = 0u;
    g->res_count       = 0u;
    g->raw_edge_count  = 0u;
    g->overflow_error  = FALSE;
    g->compiled        = FALSE;
    g->error           = GRAPH_COMPILE_OK;
    g->edge_count      = 0u;
    mem_zero_bytes(&g->stats, sizeof(g->stats));
    g->transient_mem   = NULL;
    g->transient_cap   = 0u;
    g->frame_idx       = 0u;
    g->ring_idx        = 0u;
    g->rhi_cmd         = NULL;
    atomic_store_relaxed_u32(&g->gpu_record_lock, 0u);
}

void graph_set_context(Graph* g, u32 frame_idx, u32 ring_idx, struct RhiCmdStream* rhi_cmd) {
    if (!g) return;
    g->frame_idx = frame_idx;
    g->ring_idx  = ring_idx % GRAPH_RING_SIZE;
    g->rhi_cmd   = rhi_cmd;
}

u32 graph_add_resource(Graph* g, const char* name, GraphResLifetime lifetime, u32 size, u32 align) {
    if (!g) return GRAPH_ID_NIL;
    if (g->res_count >= g->max_resources) {
        g->overflow_error = TRUE;
        g->compiled       = FALSE;
        return GRAPH_ID_NIL;
    }
    u32 id = g->res_count++;
    GraphResource* r = &g->resources[id];
    r->name       = name;
    r->lifetime   = lifetime;
    r->size       = size;
    r->align      = norm_align(align);
    r->rhi_handle = 0u;
    r->ext_ptr    = NULL;
    r->used       = FALSE;
    r->first_step = GRAPH_STEP_UNUSED;
    r->last_step  = GRAPH_STEP_UNUSED;
    r->first_node = GRAPH_ID_NIL;
    r->last_node  = GRAPH_ID_NIL;
    r->mem_offset = 0u;
    r->alias_of   = GRAPH_ID_NIL;
    r->alias_slot = GRAPH_ID_NIL;

    g->compiled = FALSE;
    return id;
}

void graph_set_resource_ext(Graph* g, u32 res_id, void* ext_ptr) {
    if (!g || res_id >= g->res_count) return;
    g->resources[res_id].ext_ptr = ext_ptr;
}

void graph_set_resource_rhi(Graph* g, u32 res_id, u32 rhi_handle) {
    if (!g || res_id >= g->res_count) return;
    g->resources[res_id].rhi_handle = rhi_handle;
}

u32 graph_add_node(Graph* g, const char* name, GraphNodeType type, GraphNodeFn fn, void* user_data) {
    if (!g) return GRAPH_ID_NIL;
    if (g->node_count >= g->max_nodes) {
        g->overflow_error = TRUE;
        g->compiled       = FALSE;
        return GRAPH_ID_NIL;
    }
    u32 id = g->node_count++;
    GraphNode* n = &g->nodes[id];
    n->name            = name;
    n->type            = type;
    n->fn              = fn;
    n->user_data       = user_data;
    n->frame_idx       = g->frame_idx;
    n->ring_idx        = g->ring_idx;
    n->rhi_cmd         = g->rhi_cmd;
    n->read_count      = 0u;
    n->write_count     = 0u;
    n->topo_step       = GRAPH_STEP_UNUSED;
    n->in_degree       = 0u;
    n->out_offset      = 0u;
    n->out_count       = 0u;
    n->cpu_to_gpu_deps = 0u;

    g->compiled = FALSE;
    return id;
}

static b32 node_has_res(const u32* arr, u32 count, u32 res_id) {
    for (u32 i = 0; i < count; i++) {
        if (arr[i] == res_id) return TRUE;
    }
    return FALSE;
}

b32 graph_node_read(Graph* g, u32 node_id, u32 res_id) {
    if (!g || node_id >= g->node_count || res_id >= g->res_count) {
        if (g) g->overflow_error = TRUE;
        return FALSE;
    }
    GraphNode* n = &g->nodes[node_id];
    if (node_has_res(n->reads, n->read_count, res_id)) {
        return TRUE;
    }
    if (n->read_count >= GRAPH_MAX_NODE_READS) {
        g->overflow_error = TRUE;
        g->compiled       = FALSE;
        return FALSE;
    }
    n->reads[n->read_count++] = res_id;
    g->compiled = FALSE;
    return TRUE;
}

b32 graph_node_write(Graph* g, u32 node_id, u32 res_id) {
    if (!g || node_id >= g->node_count || res_id >= g->res_count) {
        if (g) g->overflow_error = TRUE;
        return FALSE;
    }
    GraphNode* n = &g->nodes[node_id];
    if (node_has_res(n->writes, n->write_count, res_id)) {
        return TRUE;
    }
    if (n->write_count >= GRAPH_MAX_NODE_WRITES) {
        g->overflow_error = TRUE;
        g->compiled       = FALSE;
        return FALSE;
    }
    n->writes[n->write_count++] = res_id;
    g->compiled = FALSE;
    return TRUE;
}

b32 graph_add_edge(Graph* g, u32 from_node, u32 to_node) {
    if (!g || from_node >= g->node_count || to_node >= g->node_count) {
        if (g) g->overflow_error = TRUE;
        return FALSE;
    }
    if (g->raw_edge_count >= g->raw_edge_cap) {
        g->overflow_error = TRUE;
        g->compiled       = FALSE;
        return FALSE;
    }
    g->raw_edge_from[g->raw_edge_count] = from_node;
    g->raw_edge_to[g->raw_edge_count]   = to_node;
    g->raw_edge_count++;
    g->compiled = FALSE;
    return TRUE;
}

/* ============================================================================
 * Derleme Yardımcıları: CSR İnşası, Topolojik Sıralama ve Takma Adlama (Aliasing)
 * ============================================================================ */

typedef struct {
    u32* from;
    u32* to;
    u32  count;
    u32  cap;
    b32  overflow;
} EdgeBuf;

static void edge_buf_push(EdgeBuf* eb, u32 u, u32 v) {
    if (u == v) return;
    if (eb->count >= eb->cap) {
        eb->overflow = TRUE;
        return;
    }
    eb->from[eb->count] = u;
    eb->to[eb->count]   = v;
    eb->count++;
}

/* Ham kenar listesinden tekilleştirilmiş CSR komşuluk tablosu (out_offset, out_count, edge_targets)
 * ve her düğümün gelen kenar derecesini (in_degree) O(V + E) zamanda kurar.
 */
static b32 build_csr_edges(Graph* g, const EdgeBuf* eb, u32* scratch_counts,
                           u32* scratch_offsets, u32* scratch_sorted_to, u32* scratch_seen) {
    u32 n = g->node_count;
    for (u32 u = 0; u < n; u++) {
        scratch_counts[u]      = 0u;
        scratch_seen[u]        = 0u;
        g->nodes[u].in_degree  = 0u;
        g->nodes[u].out_offset = 0u;
        g->nodes[u].out_count  = 0u;
    }

    for (u32 i = 0; i < eb->count; i++) {
        scratch_counts[eb->from[i]]++;
    }

    u32 total = 0u;
    for (u32 u = 0; u < n; u++) {
        scratch_offsets[u] = total;
        total += scratch_counts[u];
        scratch_counts[u] = scratch_offsets[u];
    }

    for (u32 i = 0; i < eb->count; i++) {
        u32 u = eb->from[i];
        u32 pos = scratch_counts[u]++;
        scratch_sorted_to[pos] = eb->to[i];
    }

    u32 unique_edges = 0u;
    for (u32 u = 0; u < n; u++) {
        u32 start = scratch_offsets[u];
        u32 end   = scratch_counts[u];
        u32 stamp = u + 1u;

        g->nodes[u].out_offset = unique_edges;
        u32 out_c = 0u;
        for (u32 i = start; i < end; i++) {
            u32 v = scratch_sorted_to[i];
            if (scratch_seen[v] != stamp) {
                scratch_seen[v] = stamp;
                if (unique_edges >= g->edge_cap) {
                    return FALSE;
                }
                g->edge_targets[unique_edges++] = v;
                g->nodes[v].in_degree++;
                out_c++;
            }
        }
        g->nodes[u].out_count = out_c;
    }

    g->edge_count = unique_edges;
    return TRUE;
}

b32 graph_compile(Graph* g) {
    if (!g) return FALSE;
    g->compiled = FALSE;
    mem_zero_bytes(&g->stats, sizeof(g->stats));
    g->stats.node_count = g->node_count;
    g->stats.res_count  = g->res_count;

    if (g->overflow_error) {
        g->error = GRAPH_COMPILE_ERR_OVERFLOW;
        return FALSE;
    }

    for (u32 r = 0; r < g->res_count; r++) {
        GraphResource* res = &g->resources[r];
        res->used       = FALSE;
        res->first_step = GRAPH_STEP_UNUSED;
        res->last_step  = GRAPH_STEP_UNUSED;
        res->first_node = GRAPH_ID_NIL;
        res->last_node  = GRAPH_ID_NIL;
        res->mem_offset = 0u;
        res->alias_of   = GRAPH_ID_NIL;
        res->alias_slot = GRAPH_ID_NIL;
    }

    if (g->node_count == 0u) {
        g->edge_count = 0u;
        g->error      = GRAPH_COMPILE_OK;
        g->compiled   = TRUE;
        return TRUE;
    }

    /* Açık kenarlarda öz-döngü (u -> u) kontrolü */
    for (u32 i = 0; i < g->raw_edge_count; i++) {
        if (g->raw_edge_from[i] == g->raw_edge_to[i]) {
            g->error = GRAPH_COMPILE_ERR_CYCLE;
            return FALSE;
        }
    }

    usize mark = arena_mark(g->arena);
    u32 n_nodes = g->node_count;
    u32 n_res   = g->res_count;

    u32 max_gen_edges = g->raw_edge_count
                      + n_nodes * (GRAPH_MAX_NODE_READS + GRAPH_MAX_NODE_WRITES) * 3u
                      + n_res * 4u + 64u;
    EdgeBuf eb;
    eb.from     = ARENA_PUSH_ARRAY_RAW(g->arena, u32, max_gen_edges);
    eb.to       = ARENA_PUSH_ARRAY_RAW(g->arena, u32, max_gen_edges);
    eb.count    = 0u;
    eb.cap      = max_gen_edges;
    eb.overflow = FALSE;

    for (u32 i = 0; i < g->raw_edge_count; i++) {
        edge_buf_push(&eb, g->raw_edge_from[i], g->raw_edge_to[i]);
    }

    /* Kaynak okuma/yazma bildirimlerinden bağımlılık kenarlarını üret (O(N + R + erişimler)) */
    if (n_res > 0u) {
        u32* first_writer = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_res);
        u32* last_writer  = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_res);
        u32* rdr_head     = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_res);
        u32* early_head   = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_res);

        u32  max_links    = n_nodes * GRAPH_MAX_NODE_READS + 16u;
        u32* link_node    = ARENA_PUSH_ARRAY_RAW(g->arena, u32, max_links);
        u32* link_next    = ARENA_PUSH_ARRAY_RAW(g->arena, u32, max_links);
        u32  link_count   = 0u;

        for (u32 r = 0; r < n_res; r++) {
            first_writer[r] = GRAPH_ID_NIL;
            last_writer[r]  = GRAPH_ID_NIL;
            rdr_head[r]     = GRAPH_ID_NIL;
            early_head[r]   = GRAPH_ID_NIL;
        }

        /* 1. Geçiş: her kaynağın ilk üretici (pure writer, yoksa ilk writer) düğümünü bul */
        for (u32 u = 0; u < n_nodes; u++) {
            const GraphNode* nd = &g->nodes[u];
            for (u32 w = 0; w < nd->write_count; w++) {
                u32 r = nd->writes[w];
                if (first_writer[r] == GRAPH_ID_NIL && !node_has_res(nd->reads, nd->read_count, r)) {
                    first_writer[r] = u;
                }
            }
        }
        for (u32 u = 0; u < n_nodes; u++) {
            const GraphNode* nd = &g->nodes[u];
            for (u32 w = 0; w < nd->write_count; w++) {
                u32 r = nd->writes[w];
                if (first_writer[r] == GRAPH_ID_NIL) {
                    first_writer[r] = u;
                }
            }
        }

        /* 2. Geçiş: RAW, WAR ve WAW kenarlarını oluştur */
        for (u32 u = 0; u < n_nodes; u++) {
            const GraphNode* nd = &g->nodes[u];

            /* Sadece okunan (aynı düğümde yazılmayan) kaynaklar */
            for (u32 i = 0; i < nd->read_count; i++) {
                u32 r = nd->reads[i];
                if (node_has_res(nd->writes, nd->write_count, r)) {
                    continue;
                }
                if (last_writer[r] != GRAPH_ID_NIL) {
                    edge_buf_push(&eb, last_writer[r], u);
                    if (link_count < max_links) {
                        u32 idx        = link_count++;
                        link_node[idx] = u;
                        link_next[idx] = rdr_head[r];
                        rdr_head[r]    = idx;
                    }
                } else if (first_writer[r] != GRAPH_ID_NIL) {
                    /* Okuyucu düğüm üreticiden önce bildirilmiş: üretici -> okuyucu */
                    edge_buf_push(&eb, first_writer[r], u);
                    if (link_count < max_links) {
                        u32 idx        = link_count++;
                        link_node[idx] = u;
                        link_next[idx] = early_head[r];
                        early_head[r]  = idx;
                    }
                } else {
                    if (link_count < max_links) {
                        u32 idx        = link_count++;
                        link_node[idx] = u;
                        link_next[idx] = rdr_head[r];
                        rdr_head[r]    = idx;
                    }
                }
            }

            /* Yazılan (veya oku-değiştir-yaz yapılan) kaynaklar */
            for (u32 i = 0; i < nd->write_count; i++) {
                u32 r = nd->writes[i];
                if (last_writer[r] != GRAPH_ID_NIL && last_writer[r] != u) {
                    edge_buf_push(&eb, last_writer[r], u);
                } else if (last_writer[r] == GRAPH_ID_NIL &&
                           first_writer[r] != GRAPH_ID_NIL &&
                           first_writer[r] != u) {
                    edge_buf_push(&eb, first_writer[r], u);
                }

                /* Önceki sürümün tüm okuyucuları bu yazmadan önce bitmeli (WAR) */
                u32 cur = rdr_head[r];
                while (cur != GRAPH_ID_NIL) {
                    u32 rd = link_node[cur];
                    if (rd != u) {
                        edge_buf_push(&eb, rd, u);
                    }
                    cur = link_next[cur];
                }
                rdr_head[r] = GRAPH_ID_NIL;

                /* Eğer bu düğüm ilk üretici ise, kendisinden önce bildirilmiş okuyucuları
                 * aktif okuyucu listesine taşı ki sonraki yazıcılar (varsa) onları beklesin. */
                if (u == first_writer[r]) {
                    rdr_head[r]   = early_head[r];
                    early_head[r] = GRAPH_ID_NIL;
                }

                last_writer[r] = u;
            }
        }
    }

    if (eb.overflow) {
        arena_pop_to(g->arena, mark);
        g->error = GRAPH_COMPILE_ERR_OVERFLOW;
        return FALSE;
    }

    u32* sc_counts    = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_nodes);
    u32* sc_offsets   = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_nodes);
    u32* sc_sorted_to = ARENA_PUSH_ARRAY_RAW(g->arena, u32, eb.count + 1u);
    u32* sc_seen      = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_nodes);

    if (!build_csr_edges(g, &eb, sc_counts, sc_offsets, sc_sorted_to, sc_seen)) {
        arena_pop_to(g->arena, mark);
        g->error = GRAPH_COMPILE_ERR_OVERFLOW;
        return FALSE;
    }

    /* Kahn topolojik sıralama + döngü (cycle) tespiti */
    u32* deg   = sc_counts;
    u32* queue = sc_offsets;
    u32  q_head = 0u, q_tail = 0u;

    for (u32 u = 0; u < n_nodes; u++) {
        deg[u] = g->nodes[u].in_degree;
        if (deg[u] == 0u) {
            queue[q_tail++] = u;
        }
    }

    u32 topo_len = 0u;
    while (q_head < q_tail) {
        u32 u = queue[q_head++];
        g->topo_order[topo_len] = u;
        g->nodes[u].topo_step   = topo_len;
        topo_len++;

        u32 end = g->nodes[u].out_offset + g->nodes[u].out_count;
        for (u32 e = g->nodes[u].out_offset; e < end; e++) {
            u32 v = g->edge_targets[e];
            deg[v]--;
            if (deg[v] == 0u) {
                queue[q_tail++] = v;
            }
        }
    }

    if (topo_len != n_nodes) {
        arena_pop_to(g->arena, mark);
        g->error = GRAPH_COMPILE_ERR_CYCLE;
        return FALSE;
    }

    /* Paralel DAG içinde güvenli takma adlama (aliasing) için erişilebilirlik (ancestor) bit kümeleri:
     * Bir geçici kaynak R_b'nin R_a'nın belleğini yeniden kullanabilmesi (ve paralel dalları
     * yapay olarak serileştirmemesi) için R_a'yı kullanan her düğümün DAG'da R_b'yi kullanan
     * her düğümün öncülü (ancestor) olması gerekir. */
    u32 bit_words = (n_nodes + 63u) >> 6u;
    if (bit_words == 0u) bit_words = 1u;

    u64* node_anc       = ARENA_PUSH_ARRAY(g->arena, u64, (usize)n_nodes * bit_words);
    u64* res_user_mask  = ARENA_PUSH_ARRAY(g->arena, u64, (usize)(n_res + 1u) * bit_words);
    u64* res_common_anc = ARENA_PUSH_ARRAY_RAW(g->arena, u64, (usize)(n_res + 1u) * bit_words);

    for (u32 step = 0; step < n_nodes; step++) {
        u32 u = g->topo_order[step];
        const u64* anc_u = &node_anc[(usize)u * bit_words];
        u32 end = g->nodes[u].out_offset + g->nodes[u].out_count;
        for (u32 e = g->nodes[u].out_offset; e < end; e++) {
            u32 v = g->edge_targets[e];
            u64* anc_v = &node_anc[(usize)v * bit_words];
            for (u32 w = 0; w < bit_words; w++) {
                anc_v[w] |= anc_u[w];
            }
            anc_v[u >> 6u] |= (1ULL << (u & 63u));
        }
    }

    for (u32 r = 0; r < n_res; r++) {
        u64* ca = &res_common_anc[(usize)r * bit_words];
        for (u32 w = 0; w < bit_words; w++) {
            ca[w] = ~0ULL;
        }
    }

    /* Kaynak ömür aralıklarını [first_step, last_step] ve kullanıcı/öncül maskelerini hesapla */
    for (u32 step = 0; step < n_nodes; step++) {
        u32 u = g->topo_order[step];
        const GraphNode* nd = &g->nodes[u];
        const u64* anc_u = &node_anc[(usize)u * bit_words];

        for (u32 i = 0; i < nd->read_count; i++) {
            u32 r = nd->reads[i];
            GraphResource* res = &g->resources[r];
            if (!res->used) {
                res->used       = TRUE;
                res->first_step = step;
                res->first_node = u;
            }
            res->last_step = step;
            res->last_node = u;
            res_user_mask[(usize)r * bit_words + (u >> 6u)] |= (1ULL << (u & 63u));
            u64* ca = &res_common_anc[(usize)r * bit_words];
            for (u32 w = 0; w < bit_words; w++) {
                ca[w] &= anc_u[w];
            }
        }
        for (u32 i = 0; i < nd->write_count; i++) {
            u32 r = nd->writes[i];
            GraphResource* res = &g->resources[r];
            if (!res->used) {
                res->used       = TRUE;
                res->first_step = step;
                res->first_node = u;
            }
            res->last_step = step;
            res->last_node = u;
            res_user_mask[(usize)r * bit_words + (u >> 6u)] |= (1ULL << (u & 63u));
            u64* ca = &res_common_anc[(usize)r * bit_words];
            for (u32 w = 0; w < bit_words; w++) {
                ca[w] &= anc_u[w];
            }
        }
    }

    /* Geçici (transient) kaynaklar için bellek takma adlama (aliasing) planı */
    u32* trans_ids   = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_res + 1u);
    u32* trans_sizes = ARENA_PUSH_ARRAY_RAW(g->arena, u32, n_res + 1u);
    u32  trans_count = 0u;

    for (u32 r = 0; r < n_res; r++) {
        const GraphResource* res = &g->resources[r];
        if (res->used && res->lifetime == GRAPH_RES_TRANSIENT) {
            u32 sz = res->size > 0u ? res->size : GRAPH_DEFAULT_ALIGN;
            sz = (u32)ALIGN_UP(sz, res->align);
            trans_ids[trans_count] = r;
            trans_sizes[r]         = sz;
            trans_count++;
        }
    }

    /* first_step artan (eşitlikte boyut azalan, sonra kimlik artan) sıraya diz */
    for (u32 i = 1; i < trans_count; i++) {
        u32 rid = trans_ids[i];
        u32 f_i = g->resources[rid].first_step;
        u32 s_i = trans_sizes[rid];
        i32 j   = (i32)i - 1;
        while (j >= 0) {
            u32 rj  = trans_ids[j];
            u32 f_j = g->resources[rj].first_step;
            u32 s_j = trans_sizes[rj];
            if (f_j < f_i || (f_j == f_i && (s_j > s_i || (s_j == s_i && rj < rid)))) {
                break;
            }
            trans_ids[j + 1] = trans_ids[j];
            j--;
        }
        trans_ids[j + 1] = rid;
    }

    /* Takma ad yuvası (slot) tablosu: ömrü bitmiş ve DAG'da tamamlanmış yuvalar yeniden kullanılır */
    u32* slot_off  = ARENA_PUSH_ARRAY_RAW(g->arena, u32, trans_count + 1u);
    u32* slot_cap  = ARENA_PUSH_ARRAY_RAW(g->arena, u32, trans_count + 1u);
    u32* slot_last = ARENA_PUSH_ARRAY_RAW(g->arena, u32, trans_count + 1u);
    u32* slot_res  = ARENA_PUSH_ARRAY_RAW(g->arena, u32, trans_count + 1u);
    u64* slot_users = ARENA_PUSH_ARRAY(g->arena, u64, (usize)(trans_count + 1u) * bit_words);
    u32  slot_count = 0u;
    u32  peak_bytes = 0u;

    for (u32 i = 0; i < trans_count; i++) {
        u32 rid = trans_ids[i];
        GraphResource* res = &g->resources[rid];
        u32 sz  = trans_sizes[rid];
        u32 aln = res->align;
        const u64* ca_cur = &res_common_anc[(usize)rid * bit_words];
        const u64* um_cur = &res_user_mask[(usize)rid * bit_words];

        g->stats.transient_used_count++;
        g->stats.transient_unaliased_bytes += sz;

        /* Uygun (ömrü bitmiş, hizalaması ve boyutu yeten, DAG'da tüm önceki kullanıcıları
         * tamamlanmış) en küçük kapasiteli mevcut yuvayı (best-fit) ara. */
        u32 best_slot = GRAPH_ID_NIL;
        u32 best_cap  = U32_MAX;
        for (u32 s = 0; s < slot_count; s++) {
            if (slot_last[s] >= res->first_step) continue;
            if ((slot_off[s] & (aln - 1u)) != 0u) continue;
            if (slot_cap[s] < sz) continue;

            const u64* su = &slot_users[(usize)s * bit_words];
            b32 ordered = TRUE;
            for (u32 w = 0; w < bit_words; w++) {
                if ((su[w] & ~ca_cur[w]) != 0ULL) {
                    ordered = FALSE;
                    break;
                }
            }
            if (!ordered) continue;

            if (slot_cap[s] < best_cap) {
                best_cap  = slot_cap[s];
                best_slot = s;
                if (best_cap == sz) break;
            }
        }

        if (best_slot != GRAPH_ID_NIL) {
            res->mem_offset      = slot_off[best_slot];
            res->alias_of        = slot_res[best_slot];
            res->alias_slot      = best_slot;
            slot_last[best_slot] = res->last_step;
            slot_res[best_slot]  = rid;
            u64* su = &slot_users[(usize)best_slot * bit_words];
            for (u32 w = 0; w < bit_words; w++) {
                su[w] |= um_cur[w];
            }
            g->stats.transient_alias_count++;
        } else {
            u32 s   = slot_count++;
            u32 off = (u32)ALIGN_UP(peak_bytes, aln);
            slot_off[s]  = off;
            slot_cap[s]  = sz;
            slot_last[s] = res->last_step;
            slot_res[s]  = rid;
            u64* su = &slot_users[(usize)s * bit_words];
            for (u32 w = 0; w < bit_words; w++) {
                su[w] = um_cur[w];
            }
            peak_bytes = off + sz;

            res->mem_offset = off;
            res->alias_of   = GRAPH_ID_NIL;
            res->alias_slot = s;
        }
    }

    g->stats.transient_slot_count = slot_count;
    g->stats.transient_peak_bytes = peak_bytes;

    /* CPU -> GPU bağımlılık geçişlerini (bariyerlerini) say */
    for (u32 u = 0; u < n_nodes; u++) {
        g->nodes[u].cpu_to_gpu_deps = 0u;
    }
    for (u32 u = 0; u < n_nodes; u++) {
        b32 u_is_cpu = (g->nodes[u].type == GRAPH_NODE_CPU);
        u32 end = g->nodes[u].out_offset + g->nodes[u].out_count;
        for (u32 e = g->nodes[u].out_offset; e < end; e++) {
            u32 v = g->edge_targets[e];
            b32 v_is_gpu = (g->nodes[v].type != GRAPH_NODE_CPU);
            if (u_is_cpu && v_is_gpu) {
                g->nodes[v].cpu_to_gpu_deps++;
                g->stats.cpu_to_gpu_barriers++;
            }
        }
    }

    g->stats.edge_count = g->edge_count;
    arena_pop_to(g->arena, mark);

    g->error    = GRAPH_COMPILE_OK;
    g->compiled = TRUE;
    return TRUE;
}

void graph_bind_transient_memory(Graph* g, void* mem, usize cap) {
    if (!g) return;
    g->transient_mem = (u8*)mem;
    g->transient_cap = cap;
}

u8* graph_alloc_transient_memory(Graph* g, Arena* a) {
    if (!g || !g->compiled || !a) return NULL;
    usize need = (usize)g->stats.transient_peak_bytes;
    if (need == 0u) {
        g->transient_mem = NULL;
        g->transient_cap = 0u;
        return NULL;
    }
    u8* mem = (u8*)arena_push(a, need, GRAPH_DEFAULT_ALIGN);
    g->transient_mem = mem;
    g->transient_cap = need;
    return mem;
}

void* graph_res_ptr(const Graph* g, u32 res_id) {
    if (!g || res_id >= g->res_count) return NULL;
    const GraphResource* r = &g->resources[res_id];
    if (r->lifetime == GRAPH_RES_PERSISTENT) {
        return r->ext_ptr;
    }
    if (!g->compiled || !r->used || !g->transient_mem) return NULL;
    if ((usize)r->mem_offset + (usize)r->size > g->transient_cap) return NULL;
    return g->transient_mem + r->mem_offset;
}

/* ============================================================================
 * Yürütme: Seri Referans ve MPMC İş Kuyruğu Üzerinde Bariyersiz Paralel Yürütme
 * ============================================================================ */

b32 graph_execute_serial(Graph* g) {
    if (!g || !g->compiled) return FALSE;
    for (u32 step = 0; step < g->node_count; step++) {
        u32        u = g->topo_order[step];
        GraphNode* n = &g->nodes[u];
        if (n->fn) {
            GraphTaskCtx ctx;
            ctx.graph     = g;
            ctx.node_id   = u;
            ctx.frame_idx = n->frame_idx;
            ctx.ring_idx  = n->ring_idx;
            ctx.rhi_cmd   = n->rhi_cmd;
            ctx.user_data = n->user_data;
            n->fn(&ctx);
        }
    }
    return TRUE;
}

static void graph_node_job_fn(void* arg) {
    GraphJobItem* item = (GraphJobItem*)arg;
    Graph*        g    = item->g;
    u32           u    = item->node_id;
    GraphNode*    n    = &g->nodes[u];

    if (n->fn) {
        GraphTaskCtx ctx;
        ctx.graph     = g;
        ctx.node_id   = u;
        ctx.frame_idx = n->frame_idx;
        ctx.ring_idx  = n->ring_idx;
        ctx.rhi_cmd   = n->rhi_cmd;
        ctx.user_data = n->user_data;

        if (n->type == GRAPH_NODE_CPU) {
            n->fn(&ctx);
        } else {
            /* GPU düğümleri (gpu_pass, gpu_compute, transfer): RHI komut akışına
             * kayıt yaparken tek iş parçacığı kilidiyle korunur. */
            while (!atomic_cas_u32(&g->gpu_record_lock, 0u, 1u)) {
                atomic_spin_pause();
            }
            n->fn(&ctx);
            atomic_store_release_u32(&g->gpu_record_lock, 0u);
        }
    }

    /* Ardıl düğümlerin bağımlılık sayacını atomik olarak azalt;
     * sayacı 0'a düşen (hazır olan) düğümü bariyer beklemeden hemen kuyruğa gönder. */
    GraphJobItem* items = (GraphJobItem*)g->job_items;
    u32 end = n->out_offset + n->out_count;
    for (u32 e = n->out_offset; e < end; e++) {
        u32 v = g->edge_targets[e];
        if (atomic_sub_u32(&g->rem_deps[v], 1u) == 1u) {
            job_submit(graph_node_job_fn, &items[v], item->counter);
        }
    }
}

b32 graph_execute(Graph* g) {
    if (!g || !g->compiled) return FALSE;
    if (g->node_count == 0u) return TRUE;

    JobCounter counter;
    /* Kök düğümler gönderilirken sayacın erken 0'a düşmemesi için +1 nöbetçi ile başla */
    job_counter_init(&counter, 1u);

    GraphJobItem* items = (GraphJobItem*)g->job_items;
    for (u32 u = 0; u < g->node_count; u++) {
        items[u].g       = g;
        items[u].counter = &counter;
        items[u].node_id = u;
        atomic_store_relaxed_u32(&g->rem_deps[u], g->nodes[u].in_degree);
    }
    atomic_store_relaxed_u32(&g->gpu_record_lock, 0u);
    atomic_fence();

    for (u32 u = 0; u < g->node_count; u++) {
        if (g->nodes[u].in_degree == 0u) {
            job_submit(graph_node_job_fn, &items[u], &counter);
        }
    }

    /* Nöbetçi +1'i kaldır ve tüm grafik tamamlanana kadar iş çekerek bekle */
    atomic_sub_u32(&counter.pending, 1u);
    job_wait(&counter);
    return TRUE;
}

/* ============================================================================
 * 3'lü Kare Halkası ve Kare Boru Hattı (sim N+1 || render N || gpu N-1)
 * ============================================================================ */

void graph_pipeline_init(GraphPipeline* pipe, Arena* parent_arena, usize slot_arena_bytes,
                         GraphStageBuildFn build_sim, GraphStageBuildFn build_render,
                         GraphStageBuildFn build_gpu, void* user_data) {
    if (!pipe || !parent_arena) return;
    mem_zero_bytes(pipe, sizeof(*pipe));

    if (slot_arena_bytes < KB(16)) {
        slot_arena_bytes = KB(16);
    }

    for (u32 s = 0; s < GRAPH_RING_SIZE; s++) {
        pipe->slots[s].slot_arena    = arena_sub(parent_arena, slot_arena_bytes);
        pipe->slots[s].transient_buf = NULL;
        pipe->slots[s].transient_cap = 0u;
        pipe->slots[s].active_frame  = GRAPH_ID_NIL;
        pipe->slots[s].rhi_cmd       = NULL;
    }

    usize work_cap = MB(1) + slot_arena_bytes;
    pipe->work_arena = arena_sub(parent_arena, work_cap);
    graph_init(&pipe->tick_graph, &pipe->work_arena, GRAPH_MAX_NODES, GRAPH_MAX_RESOURCES);

    pipe->build_sim    = build_sim;
    pipe->build_render = build_render;
    pipe->build_gpu    = build_gpu;
    pipe->user_data    = user_data;
    pipe->tick_count   = 0u;
}

void graph_pipeline_reset(GraphPipeline* pipe) {
    if (!pipe) return;
    for (u32 s = 0; s < GRAPH_RING_SIZE; s++) {
        arena_reset(&pipe->slots[s].slot_arena);
        pipe->slots[s].transient_buf = NULL;
        pipe->slots[s].transient_cap = 0u;
        pipe->slots[s].active_frame  = GRAPH_ID_NIL;
    }
    graph_reset(&pipe->tick_graph);
    pipe->tick_count = 0u;
}

void graph_pipeline_set_rhi_stream(GraphPipeline* pipe, u32 ring_idx, struct RhiCmdStream* stream) {
    if (!pipe || ring_idx >= GRAPH_RING_SIZE) return;
    pipe->slots[ring_idx].rhi_cmd = stream;
}

b32 graph_pipeline_step(GraphPipeline* pipe, u32 tick_idx, u32 total_frames) {
    if (!pipe || total_frames == 0u) return FALSE;

    Graph* g = &pipe->tick_graph;
    usize mark = arena_mark(&pipe->work_arena);
    graph_reset(g);

    /* 1) Kare N+1 (f_sim = tick_idx): Simülasyon aşaması (halka yuvası yenilenir) */
    if (tick_idx < total_frames && pipe->build_sim) {
        u32 f_sim = tick_idx;
        u32 ring  = f_sim % GRAPH_RING_SIZE;
        GraphFrameSlot* slot = &pipe->slots[ring];
        arena_reset(&slot->slot_arena);
        slot->active_frame = f_sim;

        graph_set_context(g, f_sim, ring, slot->rhi_cmd);
        pipe->build_sim(g, f_sim, ring, &slot->slot_arena, pipe->user_data);
    }

    /* 2) Kare N (f_render = tick_idx - 1): Render / RHI kayıt aşaması */
    if (tick_idx >= 1u && (tick_idx - 1u) < total_frames && pipe->build_render) {
        u32 f_render = tick_idx - 1u;
        u32 ring     = f_render % GRAPH_RING_SIZE;
        GraphFrameSlot* slot = &pipe->slots[ring];

        graph_set_context(g, f_render, ring, slot->rhi_cmd);
        pipe->build_render(g, f_render, ring, &slot->slot_arena, pipe->user_data);
    }

    /* 3) Kare N-1 (f_gpu = tick_idx - 2): GPU yürütme / aktarım tamamlanma aşaması */
    if (pipe->build_gpu && tick_idx >= 2u && (tick_idx - 2u) < total_frames) {
        u32 f_gpu = tick_idx - 2u;
        u32 ring  = f_gpu % GRAPH_RING_SIZE;
        GraphFrameSlot* slot = &pipe->slots[ring];

        graph_set_context(g, f_gpu, ring, slot->rhi_cmd);
        pipe->build_gpu(g, f_gpu, ring, &slot->slot_arena, pipe->user_data);
    }

    if (!graph_compile(g)) {
        arena_pop_to(&pipe->work_arena, mark);
        return FALSE;
    }

    if (g->stats.transient_peak_bytes > 0u) {
        graph_alloc_transient_memory(g, &pipe->work_arena);
    }

    b32 ok = graph_execute(g);
    arena_pop_to(&pipe->work_arena, mark);
    pipe->tick_count++;
    return ok;
}

b32 graph_pipeline_run_pipelined(GraphPipeline* pipe, u32 total_frames) {
    if (!pipe) return FALSE;
    if (total_frames == 0u) return TRUE;

    u32 extra_ticks = pipe->build_gpu ? 2u : (pipe->build_render ? 1u : 0u);
    u32 total_ticks = total_frames + extra_ticks;

    for (u32 t = 0; t < total_ticks; t++) {
        if (!graph_pipeline_step(pipe, t, total_frames)) {
            return FALSE;
        }
    }
    return TRUE;
}

b32 graph_pipeline_run_serial_frames(GraphPipeline* pipe, u32 total_frames) {
    if (!pipe) return FALSE;
    if (total_frames == 0u) return TRUE;

    Graph* g = &pipe->tick_graph;
    for (u32 f = 0; f < total_frames; f++) {
        u32 ring = f % GRAPH_RING_SIZE;
        GraphFrameSlot* slot = &pipe->slots[ring];
        arena_reset(&slot->slot_arena);
        slot->active_frame = f;

        usize mark = arena_mark(&pipe->work_arena);
        graph_reset(g);
        graph_set_context(g, f, ring, slot->rhi_cmd);

        u32 sim_start = g->node_count;
        if (pipe->build_sim) {
            pipe->build_sim(g, f, ring, &slot->slot_arena, pipe->user_data);
        }
        u32 sim_end = g->node_count;

        u32 ren_start = g->node_count;
        if (pipe->build_render) {
            pipe->build_render(g, f, ring, &slot->slot_arena, pipe->user_data);
        }
        u32 ren_end = g->node_count;

        u32 gpu_start = g->node_count;
        if (pipe->build_gpu) {
            pipe->build_gpu(g, f, ring, &slot->slot_arena, pipe->user_data);
        }
        u32 gpu_end = g->node_count;

        /* Örtüşmesiz (seri kare) modunda aynı karenin aşamaları sırayla birbirini bekler:
         * sim(f) -> render(f) -> gpu(f) */
        for (u32 u = sim_start; u < sim_end; u++) {
            for (u32 v = ren_start; v < ren_end; v++) {
                graph_add_edge(g, u, v);
            }
        }
        for (u32 u = ren_start; u < ren_end; u++) {
            for (u32 v = gpu_start; v < gpu_end; v++) {
                graph_add_edge(g, u, v);
            }
        }

        if (!graph_compile(g)) {
            arena_pop_to(&pipe->work_arena, mark);
            return FALSE;
        }
        if (g->stats.transient_peak_bytes > 0u) {
            graph_alloc_transient_memory(g, &pipe->work_arena);
        }
        if (!graph_execute(g)) {
            arena_pop_to(&pipe->work_arena, mark);
            return FALSE;
        }
        arena_pop_to(&pipe->work_arena, mark);
        pipe->tick_count++;
    }
    return TRUE;
}
