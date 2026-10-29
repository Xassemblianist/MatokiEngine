/* graph/graph.h — Kare grafiği (L3): CPU/GPU düğümleri, kaynak ömrü/takma adlama ve kare boru hattı.
 * Düğümler (cpu, gpu_pass, gpu_compute, transfer) okudukları ve yazdıkları kaynakları bildirir.
 * Derleyici (graph_compile) topolojik sırayı, bağımlılık sayaçlarını ve geçici kaynaklar için
 * bellek takma adlama (aliasing) planını üretir; döngü varsa tespit eder.
 * Yürütme (graph_execute) CPU düğümlerini mevcut MPMC iş kuyruğunda bariyersiz çalıştırır.
 */
#ifndef GRAPH_GRAPH_H
#define GRAPH_GRAPH_H

#include "core/types.h"
#include "core/arena.h"

#define GRAPH_MAX_NODES        512u
#define GRAPH_MAX_RESOURCES    512u
#define GRAPH_MAX_NODE_READS   16u
#define GRAPH_MAX_NODE_WRITES  8u
#define GRAPH_RING_SIZE        3u

#define GRAPH_ID_NIL           U32_MAX
#define GRAPH_STEP_UNUSED      U32_MAX
#define GRAPH_DEFAULT_ALIGN    16u

/* RHI komut akışı ileri bildirimi (M15_B src/gpu/rhi.h ile bağlanacak) */
struct RhiCmdStream;
typedef struct Graph Graph;

/* Düğüm yürütme hedefi / türü (ARCH_V2 §5) */
typedef enum {
    GRAPH_NODE_CPU         = 0, /* CPU iş düğümü (job sistemi üzerinde bağımlılık sayaçlı paralel) */
    GRAPH_NODE_GPU_PASS    = 1, /* GPU render geçişi (şimdilik RHI kayıt fonksiyonu çağrısı) */
    GRAPH_NODE_GPU_COMPUTE = 2, /* GPU compute geçişi (şimdilik RHI kayıt fonksiyonu çağrısı) */
    GRAPH_NODE_TRANSFER    = 3  /* CPU <-> GPU / GPU <-> GPU veri aktarımı */
} GraphNodeType;

/* Kaynak ömür türü */
typedef enum {
    GRAPH_RES_TRANSIENT  = 0, /* Kare içi geçici kaynak: ömür analizi + bellek takma adlama */
    GRAPH_RES_PERSISTENT = 1  /* Kalıcı / dış kaynak: kareler arası yaşar, takma adlanmaz */
} GraphResLifetime;

/* Derleme hata kodları */
typedef enum {
    GRAPH_COMPILE_OK           = 0,
    GRAPH_COMPILE_ERR_CYCLE    = 1, /* Bağımlılık grafiğinde döngü (cycle) var */
    GRAPH_COMPILE_ERR_OVERFLOW = 2  /* Kapasite aşımı veya geçersiz düğüm/kaynak kimliği */
} GraphCompileError;

/* Düğüm yürütme / RHI kayıt bağlamı */
typedef struct {
    Graph*               graph;
    u32                  node_id;
    u32                  frame_idx; /* Kare numarası (boru hattında N+1, N veya N-1) */
    u32                  ring_idx;  /* 3'lü kare halkası indeksi (0..2) */
    struct RhiCmdStream* rhi_cmd;   /* GPU düğümleri için RHI komut akışı (M15_B) */
    void*                user_data; /* Düğüme özel kullanıcı verisi */
} GraphTaskCtx;

/* Düğüm iş / kayıt geri çağırım imzası */
typedef void (*GraphNodeFn)(const GraphTaskCtx* ctx);

/* Grafik kaynağı tanımı ve derleme sonrası ömür / takma adlama bilgileri */
typedef struct {
    const char*      name;
    GraphResLifetime lifetime;
    u32              size;       /* Bayt cinsinden boyut */
    u32              align;      /* Bayt hizalaması (2'nin kuvveti, en az 16) */
    u32              rhi_handle; /* GPU kaynağı için RHI tutamacı (M15_B; yoksa 0) */
    void*            ext_ptr;    /* Kalıcı kaynak için dış bellek işaretçisi (isteğe bağlı) */

    /* Derleme (graph_compile) çıktıları: */
    b32              used;       /* En az bir düğüm tarafından okunuyor veya yazılıyor mu? */
    u32              first_step; /* Topolojik sırada ilk kullanım adımı (yoksa GRAPH_STEP_UNUSED) */
    u32              last_step;  /* Topolojik sırada son kullanım adımı (yoksa GRAPH_STEP_UNUSED) */
    u32              first_node; /* İlk kullanan düğüm kimliği (yoksa GRAPH_ID_NIL) */
    u32              last_node;  /* Son kullanan düğüm kimliği (yoksa GRAPH_ID_NIL) */
    u32              mem_offset; /* Geçici bellek bloğu içindeki bayt ofseti */
    u32              alias_of;   /* Belleğini devraldığı önceki geçici kaynak (yoksa GRAPH_ID_NIL) */
    u32              alias_slot; /* Takma adlama yuvası kimliği (yoksa GRAPH_ID_NIL) */
} GraphResource;

/* Grafik düğümü tanımı ve derleme sonrası topolojik / bağımlılık bilgileri */
typedef struct {
    const char*          name;
    GraphNodeType        type;
    GraphNodeFn          fn;
    void*                user_data;

    /* Düğümün eklendiği andaki kare / halka / RHI bağlamı (boru hattı birleştirmesi için) */
    u32                  frame_idx;
    u32                  ring_idx;
    struct RhiCmdStream* rhi_cmd;

    /* Bildirilen okuma / yazma kaynakları */
    u32                  reads[GRAPH_MAX_NODE_READS];
    u32                  read_count;
    u32                  writes[GRAPH_MAX_NODE_WRITES];
    u32                  write_count;

    /* Derleme (graph_compile) çıktıları: */
    u32                  topo_step;       /* Topolojik sıradaki konumu (0..node_count-1) */
    u32                  in_degree;       /* Gelen bağımlılık kenarı sayısı */
    u32                  out_offset;      /* edge_targets içindeki başlangıç ofseti */
    u32                  out_count;       /* Çıkan bağımlılık kenarı sayısı */
    u32                  cpu_to_gpu_deps; /* Bu GPU düğümünün beklediği CPU düğümü sayısı */
} GraphNode;

/* Derleme istatistikleri ve bellek takma adlama özeti */
typedef struct {
    u32 node_count;
    u32 res_count;
    u32 edge_count;
    u32 cpu_to_gpu_barriers;       /* CPU -> GPU bağımlılık kenarı sayısı */
    u32 transient_used_count;      /* Kullanılan geçici kaynak sayısı */
    u32 transient_alias_count;     /* Önceki bir geçici kaynağın belleğini paylaşan kaynak sayısı */
    u32 transient_slot_count;      /* Toplam ayrılan takma ad yuvası sayısı */
    u32 transient_unaliased_bytes; /* Takma adlama olmasaydı gereken toplam geçici bellek (bayt) */
    u32 transient_peak_bytes;      /* Takma adlama sonrası gereken tepe geçici bellek (bayt) */
} GraphStats;

struct Graph {
    Arena*               arena;
    u32                  max_nodes;
    u32                  max_resources;

    GraphNode*           nodes;
    u32                  node_count;

    GraphResource*       resources;
    u32                  res_count;

    /* Açık (explicit) eklenen kenarlar */
    u32*                 raw_edge_from;
    u32*                 raw_edge_to;
    u32                  raw_edge_count;
    u32                  raw_edge_cap;
    b32                  overflow_error;

    /* Derleme çıktıları */
    b32                  compiled;
    GraphCompileError    error;
    u32*                 topo_order;   /* [max_nodes] topolojik sıradaki düğüm kimlikleri */
    u32*                 edge_targets; /* CSR bitişiklik listesi hedef düğümleri */
    u32                  edge_count;
    u32                  edge_cap;
    GraphStats           stats;

    /* Yürütme zamanı (runtime) durumu */
    u32*                 rem_deps;        /* [max_nodes] atomik kalan bağımlılık sayaçları */
    void*                job_items;       /* [max_nodes] dahili iş kuyruğu görev tanımlayıcıları */
    u8*                  transient_mem;   /* Geçici kaynaklar için bağlanan bellek bloğu */
    usize                transient_cap;
    u32                  frame_idx;
    u32                  ring_idx;
    struct RhiCmdStream* rhi_cmd;
    _Alignas(4) u32      gpu_record_lock; /* GPU kayıt fonksiyonlarının RHI akışına sıralı erişimi */
};

/* ============================================================================
 * Kare Grafiği (Graph) Kurulum, Bildirim, Derleme ve Yürütme API'si
 * ============================================================================ */

/* Grafiği verilen kapasitelerle arena üzerinde ilklendirir. */
void   graph_init(Graph* g, Arena* a, u32 max_nodes, u32 max_resources);

/* Varsayılan kapasitelerle (GRAPH_MAX_NODES, GRAPH_MAX_RESOURCES) yeni grafik oluşturur. */
Graph* graph_create(Arena* a);

/* Grafiği yeni bir kare/inşa için sıfırlar (ayrılmış dizileri korur, düğüm/kaynak sayısını 0 yapar). */
void   graph_reset(Graph* g);

/* Sonradan eklenecek düğümler için aktif kare indeksi, halka indeksi ve RHI akışını ayarlar. */
void   graph_set_context(Graph* g, u32 frame_idx, u32 ring_idx, struct RhiCmdStream* rhi_cmd);

/* Kaynak ekler ve kaynak kimliğini (0..res_count-1) döndürür; taşmada GRAPH_ID_NIL döner. */
u32    graph_add_resource(Graph* g, const char* name, GraphResLifetime lifetime, u32 size, u32 align);

/* Kalıcı kaynak için dış bellek işaretçisi veya GPU RHI tutamacı bağlar. */
void   graph_set_resource_ext(Graph* g, u32 res_id, void* ext_ptr);
void   graph_set_resource_rhi(Graph* g, u32 res_id, u32 rhi_handle);

/* Düğüm ekler ve düğüm kimliğini (0..node_count-1) döndürür; taşmada GRAPH_ID_NIL döner. */
u32    graph_add_node(Graph* g, const char* name, GraphNodeType type, GraphNodeFn fn, void* user_data);

/* Düğümün bir kaynağı okuduğunu / yazdığını bildirir. */
b32    graph_node_read(Graph* g, u32 node_id, u32 res_id);
b32    graph_node_write(Graph* g, u32 node_id, u32 res_id);

/* İki düğüm arasına açık sıralama bağımlılığı ekler (from_node -> to_node). */
b32    graph_add_edge(Graph* g, u32 from_node, u32 to_node);

/* Grafiği derler: kenarları kurar, döngü denetimi + topolojik sıralama yapar,
 * geçici kaynak ömrü ve takma adlama (aliasing) planını hesaplar.
 * Başarılıysa TRUE, döngü veya taşma varsa FALSE döner (g->error ayarlanır).
 */
b32    graph_compile(Graph* g);

/* Derlenmiş grafiğin geçici kaynakları için bellek tamponu bağlar (veya arena'dan ayırır). */
void   graph_bind_transient_memory(Graph* g, void* mem, usize cap);
u8*    graph_alloc_transient_memory(Graph* g, Arena* a);

/* Bir kaynağın çalışma zamanı bellek adresini döndürür (kalıcıysa ext_ptr, geçiciyse transient_mem + mem_offset). */
void*  graph_res_ptr(const Graph* g, u32 res_id);

/* Derlenmiş grafiği topolojik sırada tek iş parçacığında (seri referans) çalıştırır. */
b32    graph_execute_serial(Graph* g);

/* Derlenmiş grafiği mevcut MPMC iş sistemi (job.h) üzerinde bağımlılık sayaçlarıyla
 * bariyersiz olarak paralel çalıştırır; tüm düğümler tamamlanana kadar bekler.
 */
b32    graph_execute(Graph* g);

/* ============================================================================
 * 3'lü Kare Halkası ve Kare Boru Hattı (ARCH_V2 §2, §5: sim N+1 || render N || gpu N-1)
 * ============================================================================ */

typedef enum {
    GRAPH_STAGE_SIM    = 0, /* Kare N+1: CPU simülasyon / fizik / ECS güncellemesi */
    GRAPH_STAGE_RENDER = 1, /* Kare N  : Görünürlük, çizim hazırlığı ve RHI/render kaydı */
    GRAPH_STAGE_GPU    = 2  /* Kare N-1: GPU yürütme / aktarım / sunum tamamlanması */
} GraphStage;

/* Bir aşamanın (sim, render veya gpu) düğümlerini grafiğe ekleyen geri çağırım.
 * g üzerinde graph_set_context(g, frame_idx, ring_idx, rhi_cmd) çağrılmış olarak gelir;
 * slot_arena ilgili halka yuvasının (frame_idx % 3) kare arenasıdır.
 */
typedef void (*GraphStageBuildFn)(Graph* g, u32 frame_idx, u32 ring_idx, Arena* slot_arena, void* user_data);

/* 3'lü kare halkasındaki tek bir yuva (slot 0, 1, 2) */
typedef struct {
    Arena                slot_arena;    /* Bu halkadaki karenin geçici arenası */
    u8*                  transient_buf; /* Geçici kaynaklar için halka tamponu */
    usize                transient_cap;
    u32                  active_frame;  /* Bu yuvayı en son kullanan kare indeksi */
    struct RhiCmdStream* rhi_cmd;       /* Bu yuvanın RHI komut akışı (M15_B) */
} GraphFrameSlot;

/* 3'lü kare boru hattı yöneticisi */
typedef struct {
    Arena             work_arena;              /* Birleşik tick grafiğinin derleme/çalışma arenası */
    Graph             tick_graph;              /* Her adımda derlenen/çalıştırılan grafik */
    GraphFrameSlot    slots[GRAPH_RING_SIZE];  /* 3 kare halkası (N+1 sim, N render, N-1 gpu) */
    GraphStageBuildFn build_sim;
    GraphStageBuildFn build_render;
    GraphStageBuildFn build_gpu;               /* İsteğe bağlı (NULL olabilir) */
    void*             user_data;
    u32               tick_count;              /* Şimdiye kadar yürütülen toplam boru hattı adımı */
} GraphPipeline;

/* 3'lü kare boru hattını ilklendirir; parent_arena'dan halka yuvaları ve çalışma alanı ayırır. */
void graph_pipeline_init(GraphPipeline* pipe, Arena* parent_arena, usize slot_arena_bytes,
                         GraphStageBuildFn build_sim, GraphStageBuildFn build_render,
                         GraphStageBuildFn build_gpu, void* user_data);

/* Boru hattı sayaçlarını ve halka arenalarını sıfırlar. */
void graph_pipeline_reset(GraphPipeline* pipe);

/* Halka yuvasına RHI komut akışı işaretçisi bağlar (ring_idx < 3). */
void graph_pipeline_set_rhi_stream(GraphPipeline* pipe, u32 ring_idx, struct RhiCmdStream* stream);

/* Tek bir boru hattı adımını (tick) inşa eder, derler ve yürütür.
 * tick_idx adımında:
 *  - tick_idx < total_frames           ise: sim(tick_idx)        [ring = tick_idx % 3]
 *  - 1 <= tick_idx <= total_frames     ise: render(tick_idx - 1) [ring = (tick_idx - 1) % 3]
 *  - 2 <= tick_idx <= total_frames + 1 ise: gpu(tick_idx - 2)    [ring = (tick_idx - 2) % 3] (build_gpu != NULL ise)
 * aynı grafikte bağımsız alt grafikler olarak paralel çalıştırılır.
 */
b32  graph_pipeline_step(GraphPipeline* pipe, u32 tick_idx, u32 total_frames);

/* Toplam total_frames adet kareyi boru hattı örtüşmesiyle (sim N+1 || render N || gpu N-1) yürütür. */
b32  graph_pipeline_run_pipelined(GraphPipeline* pipe, u32 total_frames);

/* Karşılaştırma ve doğrulama için: total_frames adet kareyi örtüşmesiz (her kare kendi içinde
 * sim(f) -> render(f) -> gpu(f) sıralı bağımlılık zinciriyle) yürütür.
 */
b32  graph_pipeline_run_serial_frames(GraphPipeline* pipe, u32 total_frames);

#endif /* GRAPH_GRAPH_H */
