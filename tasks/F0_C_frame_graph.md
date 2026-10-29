# F0_C — Kare grafiği v0: CPU + GPU düğümleri, kare boru hattı (H3)

Durum: DONE (agy)

## Amaç
ARCH_V2 §5. `src/graph/`: düğüm (cpu | gpu_pass | gpu_compute | transfer), kaynak bildirimi (okur/yazar, geçici/
kalıcı), derleme (topolojik sıra, bağımlılık sayaçları, geçici kaynak ömrü + takma adlama planı), yürütme: cpu
düğümleri mevcut job sistemi üzerinde bağımlılık sayaçlarıyla (bariyersiz, hazır olan çalışır); gpu düğümleri şimdilik
kayıt fonksiyonu çağrısı (RHI M15_B gelince bağlanacak — arayüzü buna uygun bırak). Kare boru hattı: sim N+1 ile render
kaydı N'nin örtüşmesi (3 kare halkası).

## Dosyalar
- `src/graph/*` (yeni), `tests/test_graph.c` (yeni), `Makefile` (yalnız yeni kaynaklar), `docs/ARCHITECTURE.md` (modül satırı)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Testler: döngü tespiti, yanlış sıra yok, ömür/alias planı doğru, rastgele 1000 grafik × yürütme = seri sonuç
- [x] Ölçüm: 200 düğümlü grafikte derleme + yürütme ek yükü (host); boru hattı kazancı (sentetik iş yüküyle)

## Notlar
- **Kare grafiği (`src/graph/graph.h`, `src/graph/graph.c`):**
  - 4 düğüm türü (`GRAPH_NODE_CPU`, `GRAPH_NODE_GPU_PASS`, `GRAPH_NODE_GPU_COMPUTE`, `GRAPH_NODE_TRANSFER`) ve 2 kaynak ömür türü (`GRAPH_RES_TRANSIENT`, `GRAPH_RES_PERSISTENT`) tanımlandı.
  - Derleme (`graph_compile`): kaynak okuma/yazma bildirimlerinden (üreticiden önce bildirilen okuyucular ve çoklu yazıcı zincirleri dahil) RAW/WAR/WAW kenarlarını ve açık kenarları tekilleştirilmiş CSR tablosuna dönüştürür; Kahn algoritması ile $O(V+E)$ topolojik sıralama ve döngü tespiti (`GRAPH_COMPILE_ERR_CYCLE`) yapar. Geçici derleme dizileri `arena_mark`/`arena_pop_to` ile geri sarılır; tekrarlı derlemede ek arena tüketimi 0 bayttır.
  - Geçici kaynak ömrü ve takma adlama (aliasing): her kaynağın `[first_step, last_step]` aralığı ile DAG üzerindeki erişilebilirlik (ancestor) bit kümeleri hesaplanır; `best-fit` yuva tablosu yalnızca topolojik ömrü bitmiş ve DAG'da tüm kullanıcıları tamamlanmış yuvaları yeniden kullanır (paralel dallar arasında yapay serileştirme veya bellek yarışı oluşmaz).
  - Yürütme (`graph_execute`): `src/thread/job.h` MPMC kuyruğu üzerinde atomik bağımlılık sayaçlarıyla (`rem_deps`) bariyersiz çalışır; biten düğüm `atomic_sub_u32(&rem_deps[v], 1) == 1` olan ardıllarını anında kuyruğa iter. GPU düğümleri `struct RhiCmdStream*` alan kayıt fonksiyonunu çağırır (`gpu_record_lock` ile RHI akışına sıralı kayıt garantisi; M15_B RHI ile doğrudan bağlanmaya hazır).
  - Kare boru hattı (`GraphPipeline`): 3 kare halkası (`GRAPH_RING_SIZE = 3`, her yuvada bağımsız `slot_arena` ve `rhi_cmd`) ile `t` adımında `sim(N+1)` (`ring = (N+1)%3`), `render(N)` (`ring = N%3`) ve isteğe bağlı `gpu(N-1)` (`ring = (N-1)%3`) alt grafiklerini tek birleşik grafikte bariyersiz örtüştürür.
- **Makefile:** `find src -name '*.c'` ve `wildcard tests/*.c` yeni dosyaları otomatik topladığı için `Makefile`'da değişikliğe gerek kalmadı.
- **Testler (`tests/test_graph.c`):**
  - `graph_cycle_detection`: öz-döngü, 2/3/4 düğümlü kaynak ve açık kenar döngüleri + geçerli elmas DAG kontrolü.
  - `graph_topological_order_and_node_types`: ters sırada bildirilmiş üretici/tüketici zinciri, çoklu yazıcı (RAW/WAR/WAW) ve `RhiCmdStream` kayıt doğrulaması.
  - `graph_transient_lifetime_and_aliasing`: 5 geçişli boru hattında ping-pong bellek yeniden kullanımı (4 × 4 KB = 16 KB yerine 2 × 4 KB = 8 KB tepe geçici bellek) ve gerçek geçici tampon üzerinden veri akışı.
  - `graph_random_1000_dags_match_serial`: 4 iş parçacığında (ana thread + 3 worker pthread) rastgele 1000 DAG (8..64 düğüm, 6..32 kaynak, karışık bildirim sırası) paralel yürütülüp seri referansla karşılaştırıldı: 0 sıra ihlali, 1000/1000 bit-bit eşleşme.
- **Ölçümler (H3 doğrulaması, host `clang -O1 -fsanitize=address,undefined`, paylaşımlı makine `uptime` load average: 5.39, 5.54, 4.79):**

  | Ölçüt | Hedef (H3) | Gözlenen (medyan) | Durum |
  |---|---|---|---|
  | 200 düğümlü grafik (60 geçici kaynak, 20 aşama) derleme | — | **0.0489 ms** | — |
  | 200 düğümlü grafik yürütme (MPMC job sistemi) | — | **0.0173 ms** | — |
  | 200 düğümlü grafik toplam (derleme + yürütme) | **< 0.1 ms** | **0.0663 ms** | **GEÇTİ** |
  | 200 düğümlü grafikte geçici bellek takma adlama | — | 61440 → **3072 bayt** (20× küçülme, 3 yuva) | — |
  | Boru hattı (24 kare, 2 aşama: `sim N+1 ∥ render N`) | **≥ 1.3×** | 5.407 ms → 2.892 ms (**1.87×**) | **GEÇTİ** |
  | Boru hattı (24 kare, 3 aşama: `sim N+1 ∥ render N ∥ gpu N-1`) | **≥ 1.3×** | 8.281 ms → 3.096 ms (**2.67×**) | **GEÇTİ** |

- **Ertelenen:** M15_B (`src/gpu/rhi.h`) birleştiğinde `struct RhiCmdStream` gerçek RHI komut akışına bağlanacak; maliyet tabanlı otomatik `cpu` ↔ `gpu_compute` seçimi Faz 2'de eklenecek.
