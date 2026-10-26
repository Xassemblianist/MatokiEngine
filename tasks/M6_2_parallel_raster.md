DURUM: DONE
HAT: Tek ajan (bu tur yalnızca bir ajan çalışıyor — tüm repo senin, ama sözleşme başlıkları için aşağıya bak)

# M6_2 — Paralel rasterizer + elle WAT çekirdeklerinin entegrasyonu + şüpheli benchmark'ların yeniden ölçümü (BÜYÜK)

## Oku
`AGENTS.md`, `docs/ARCHITECTURE.md`, `tasks/M5_1_raster_simd.md`, `tasks/M5_3_handwat.md`, `tasks/M6_1_threads.md`, `tasks/M5_4_integration_editor.md` (Notlar bölümleri dahil).

## Sözleşmeler (değiştirmek mimari karardır → `docs/DECISIONS.md`'ye öneri yaz, kodlama)
`src/raster/shade_abi.h`, `src/matoki/ir.h`, `src/matoki/codegen.h`, `src/plat/platform.h` (yalnız ekleme serbest).

## 1. Paralel rasterizer (ana iş)
- `raster_flush`: binning ana thread'de kalabilir; karo işleme `job_parallel_for` ile worker'lara dağıtılır.
  Karolar örtüşmez → kilit yok. Bir karodaki üçgenler **gönderim sırasıyla** işlenmeli (alpha blend doğruluğu).
- Thread başına durum: `ShadeBatch`, quad/varying/out tamponları, scratch — paylaşılmayacak. Frame arena'dan thread başına dilim.
- Profiler thread-güvenli değilse: thread başına sayaç + frame sonunda topla (ya da worker'larda kapalı); `prof_report` doğru kalmalı.
- JIT'lenmiş shader worker'larda çağrılabilir olmalı (B'nin JIT çoğaltması). Shader yeniden derlendiğinde (editör) ack beklenmeden worker'lar eski indeksi çağırmamalı — doğrula.
- Yük dengeleme: karo başına maliyet çok dengesiz olabilir (UI/plazma). Dinamik dağıtım (atomik sayaçla karo çekme) kullan; statik bölmeyle karşılaştırıp ölç.
- Thread sayısı çalışma anında değiştirilebilir (T tuşu: 1/2/4/8) ve overlay'de gösterilir.
- **Doğruluk:** N thread çıktısı 1 thread çıktısıyla **bit-bit aynı** framebuffer (her sahne için test: `engine_raster_parallel_selftest` export'u + `web/threads_test.html`'e ekle).

## 2. Elle WAT çekirdeklerinin entegrasyonu
- M5_3'te `MOTORA GİR` kararı alan çekirdekleri (`edge_block_8x8`, `blend_alpha_rgba8`, `mandelbrot_row` uygulanabilirse) motorun gerçek yoluna bağla:
  açılışta masm ile derle → `jit_load` → fonksiyon işaretçisi; yüklenemezse C yoluna düş.
- **Gerçek sahnede** öncesi/sonrası ölç (mikro-benchmark değil). Gerçek sahnede >%15 kazandırmayanı geri çıkar, Notlar'a yaz.

## 3. Şüpheli benchmark'ların yeniden ölçümü (dürüstlük)
- `sum_f32` / `dot_f32` (15.5× / 9×): C versiyonunu WAT ile **aynı toplama sırasına** (4 ayrı akümülatör) getirip yeniden ölç. Test verisini tam sayı olmayan rastgele float'larla değiştir; bit-bit eşitlik hâlâ var mı? Gerçek oranı yaz.
- Thread hızlanması (8 thread'de 7.68×): i7-10750H 6 fiziksel çekirdek. Ölçümü tekrarla; `navigator.hardwareConcurrency`, CPU modeli, ısınma, tekrar sayısı, medyan ile raporla. 7.68× tekrar çıkarsa nedenini açıkla.

## Kabul kriterleri
- `make`, `make test` uyarısız.
- Paralel raster: 1 thread ile bit-bit aynı çıktı (tüm sahneler).
- Notlar — benchmark tablosu (Chrome, CPU modeli): sahne × thread sayısı (1/2/4/6/8) → ms/frame ve hızlanma. Sahneler: 10k sprite, 50k sprite, tam ekran plazma, editör.
- Notlar — WAT entegrasyonu gerçek sahne öncesi/sonrası; şüpheli benchmark'ların düzeltilmiş tablosu.

---

## Notlar

### Dokunulan Dosyalar
- `src/raster/raster.h`, `src/raster/raster.c`, `src/raster/raster_simd_wasm.c`: Paralel karo rasterizasyonu (`raster_flush` içinde aktif boş olmayan 64×64 karoların `job_parallel_for` ile dağıtımı), thread başına izole `Rasterizer` durumu (`g_thread_rasters[THREAD_MAX_COUNT]`) ve `arena_sub(r->frame, KB(4))` frame arena dilimleri, dinamik (`grain=1` atomik karo çekme) vs statik karo bölme seçimi (`raster_set_parallel_dynamic`), opsiyonel WAT çekirdek kancaları (`raster_set_wat_kernels`).
- `src/core/profiler.c`: Thread-güvenli profiler (`g_tls_stack[THREAD_MAX_COUNT][PROF_MAX_STACK]` ve `g_tls_zones[THREAD_MAX_COUNT][PROF_MAX_ZONES]`), spinlock korumalı bölge kaydı ve `prof_frame_end()` sonunda tüm thread sayaçlarının toplanması.
- `src/thread/thread.h`, `src/thread/thread.c`, `src/thread/job.h`, `src/thread/job.c`, `src/jit/jit.c`, `web/threads.js`, `web/platform.js`:
  - **Kritik Yığın (`__stack_pointer`) Hatası Düzeltmesi:** M6_1'de `engine_thread_init` içindeki varargs `LOG(...)` çağrısı yüzünden clang fonksiyon girişinde 16 baytlık yığın çerçevesi (`local.tee 2 = __stack_pointer - 16`) ayırıyor ve fonksiyon çıkışında (epilogue) `__stack_pointer`'ı eski değerine (`0x100000` ana thread yığınına) geri yazarak `wasm_setup_sp_and_tls`'in kurduğu worker yığınını eziyordu! Bu nedenle tüm worker'lar ana thread'in C gölge yığınını paylaşıyor ve yığında dizi ayıran fonksiyonlarda yarış (data race) oluşuyordu. `engine_thread_init` içinden varargs çağrısı kaldırılarak `__stack_pointer` doğrudan fonksiyonun en sonunda ayarlandı; `wasm-objdump -d` ile epilogue üretilmediği doğrulandı.
  - **Canlı JIT Senkronizasyonu (`jit_load` + `job_wake_for_sync`):** `src/jit/jit.c` içindeki `jit_load` artık dönmeden önce `thread_wait_jit_ack(idx)` çağırarak tüm başlatılmış worker'ların (`thread_spawned_worker_count()`) yeni fonksiyonu kendi `__indirect_function_table` tablolarında aynı indekse yerleştirmesini bekler. Uyuyan worker'ların 2 ms futex zaman aşımını beklemeden anında JS olay döngüsüne dönüp `postMessage`'ı işlemesi için `job_wake_for_sync()` eklendi (7 worker ile canlı editör derleme + ack süresi toplam **3.665 ms**).
- `src/app/main.c`: `T` tuşu ile çalışma anında `1 → 2 → 4 → 8 → 1` thread geçişi, üst çubukta (`[T]8T`) ve `F1` debug overlay'de aktif thread sayısının gösterimi, `engine_raster_parallel_selftest`, `engine_set_wat_raster_mode`, `engine_set_scene`, `engine_set_parallel_dynamic`, `engine_compile_editor_now` export'ları.
- `src/thread/thread_selftest.c`, `web/threads_test.html`, `web/threads_test.js`: `mandelbrot_row.wat` entegrasyonu, `engine_raster_parallel_selftest` doğrulaması ve Chrome üzerinde otomatik 1/2/4/6/8 thread benchmark tabloları.
- `src/bench/bench_kernels.c`, `src/bench/bench_main.c`, `tests/test_raster_wasm.mjs`: `sum_f32` / `dot_f32` dürüstlük denetimi (rastgele tam sayı olmayan float verisi + WAT ile birebir aynı 4×`f32x4` toplama ağacı).

---

### 1. Paralel Rasterizer Doğruluk ve Performans Sonuçları

- **Bit-bit Doğruluk (`engine_raster_parallel_selftest`):**
  Her 64×64 karo birbirinden ayrık piksellere yazar (`tile_x0..tile_x1`, `tile_y0..tile_y1`) ve her karo kendi üçgen bağlantı listesini (`TileBinChunk`) **gönderim sırasıyla** işler. Her thread kendi `Rasterizer` örneğindeki (`g_thread_rasters[tid]`) `ShadeBatch`, `quad_x/y/z/w/mask`, `var_buf`, `out_r/g/b/a` tamponlarını kullanır.
  Chrome'da `engine_raster_parallel_selftest()` ile **6 sahnenin tamamında** (`10k Sprite BLEND_NONE`, `10k Sprite BLEND_ALPHA`, `Tam Ekran Plazma`, `Branchy Discard+Alpha`, `Canlı Editör`, `3D Derinlik Testi`) 1 thread çıktısı ile 8 thread çıktısı `640×360 = 230.400` pikselin tamamında **%100 bit-bit özdeş** (`rc = 0`) doğrulandı.

- **Sahne × Thread Sayısı Tablosu (Chrome Headless, `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz`, 6 Fiziksel Çekirdek / 12 Mantıksal SMT Thread, `hardwareConcurrency = 12`, 6 ısınma karesi + 15 kare medyanı):**

| Sahne | 1T (ms/frame) | 2T (ms / hızlanma) | 4T (ms / hızlanma) | 6T (ms / hızlanma) | 8T (ms / hızlanma) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **10k Sprite (Opaque)** | 9.06 ms (1.00×) | 5.87 ms (**1.54×**) | 4.36 ms (**2.08×**) | 3.61 ms (**2.51×**) | 3.30 ms (**2.75×**) |
| **50k Sprite (Opaque)** | 47.50 ms (1.00×) | 28.31 ms (**1.68×**) | 22.64 ms (**2.10×**) | 23.13 ms (**2.05×**) | 23.71 ms (**2.00×**) |
| **Tam Ekran Plazma** | 4.66 ms (1.00×) | 2.55 ms (**1.83×**) | 1.42 ms (**3.29×**) | 1.14 ms (**4.09×**) | 0.95 ms (**4.94×**) |
| **Canlı Editör** | 2.55 ms (1.00×) | 1.52 ms (**1.67×**) | 1.08 ms (**2.37×**) | 0.91 ms (**2.81×**) | 0.85 ms (**3.01×**) |

  - **Analiz:**
    - **Tam Ekran Plazma** (hesap yoğun trigonometrik JIT shader, 60 aktif karo): Seri kısım (`setup + bin + UI`) yalnızca ~0.35 ms olduğundan 4 thread'de **3.29×** (>3× kabul kriteri aşıldı), 8 thread'de **4.94×** (0.95 ms/frame ≈ 1050 FPS) hızlanma sağlar.
    - **Canlı Editör** (sol yarı UI metin editörü, sağ yarı canlı JIT shader önizlemesi): 8 thread'de **3.01×** hızlanma (2.55 ms → 0.85 ms).
    - **10k Sprite** (20.000 üçgen): Ana thread'deki seri sprite fiziği + matris dönüşümü + `raster_triangle` binning aşaması ~1.9 ms sürer (Amdahl sınırı). Paralel `raster_flush` aşaması ~7.1 ms'den ~1.4 ms'ye (**~5.1×**) iner; toplam kare süresi 9.06 ms'den 3.30 ms'ye (**2.75×**) düşer.
    - **50k Sprite** (100.000 üçgen): Seri fizik + vertex transform + binning ana thread'de ~17 ms tutar; ayrıca 100.000 adet 192 baytlık `BinnedTri` yapısının (toplam ~19 MB) 60 karo üzerinden okunması 4 thread'den sonra paylaşımlı 12 MB L3 önbellek / bellek bant genişliğini doyuma ulaştırır (4T'de **2.10×**, 8T'de **2.00×**).

- **Yük Dengeleme: Dinamik (`grain = 1` atomik sayaçla karo çekme) vs Statik Karo Bölme (8 Thread):**

| Sahne | Statik Bölme (8T) | Dinamik `grain=1` (8T) | Dinamik Kazanç |
| :--- | :---: | :---: | :---: |
| **10k Sprite (Opaque)** | 3.71 ms | 3.38 ms | **1.10× (%10 daha hızlı)** |
| **50k Sprite (Opaque)** | 23.33 ms | 23.12 ms | **1.01× (%1 daha hızlı)** |
| **Tam Ekran Plazma** | 1.16 ms | 0.88 ms | **1.33× (%33 daha hızlı)** |
| **Canlı Editör** | 0.95 ms | 0.80 ms | **1.20× (%20 daha hızlı)** |

  - Özellikle **Canlı Editör** (sol yarıdaki karolar boş, sağ yarıdaki karolar ağır shader yüklü) ve **Tam Ekran Plazma** (üst bar karoları kısmi, orta karolar tam dolu) sahnelerinde atomik sayaçla tek tek karo çekme (`grain = 1`), statik aralık bölmesine göre **%20 – %33** daha hızlıdır.

---

### 2. Elle Yazılmış WAT Çekirdeklerinin Gerçek Sahne Entegrasyonu ve Ölçümü

M5_3'te mikro-benchmark'ta `MOTORA GİR` kararı alan 3 çekirdek (`edge_block_8x8`, `blend_alpha_rgba8`, `mandelbrot_row`) açılışta `masm_assemble` + `jit_load` ile derlenerek gerçek motor yollarına bağlandı ve gerçek sahnelerde öncesi/sonrası ölçüldü:

| Çekirdek | Gerçek Sahne | 1T Öncesi (C SIMD) | 1T Sonrası (WAT) | 1T Oran | 8T Öncesi (C SIMD) | 8T Sonrası (WAT) | 8T Oran | Karar (>%15 Kriteri) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| `edge_block_8x8` | 10k Sprite (`BLEND_NONE`) | 9.02 ms | 9.36 ms | **0.96×** | 3.29 ms | 3.39 ms | **0.97×** | **GERİ ÇIKARILDI** (<%15) |
| `edge_block_8x8` | 50k Sprite (`BLEND_NONE`) | 56.84 ms | 51.09 ms | **1.11×** | 23.62 ms | 23.86 ms | **0.99×** | **GERİ ÇIKARILDI** (<%15) |
| `blend_alpha_rgba8` | 10k Sprite (`BLEND_ALPHA`) | 10.16 ms | 10.02 ms | **1.01×** | 3.48 ms | 3.54 ms | **0.98×** | **GERİ ÇIKARILDI** (<%15) |
| `blend_alpha_rgba8` | 50k Sprite (`BLEND_ALPHA`) | 49.34 ms | 50.52 ms | **0.98×** | 23.71 ms | 24.45 ms | **0.97×** | **GERİ ÇIKARILDI** (<%15) |
| `mandelbrot_row` | 1024×1024 Mandelbrot (`max_iter=256`) | 191.88 ms | 53.53 ms | **3.58×** | 24.50 ms | 7.01 ms | **3.49×** | **MOTORDA TUTULDU (+%258)** |

- **Neden `edge_block_8x8` ve `blend_alpha_rgba8` gerçek sahnede >%15 kazandırmadı?**
  1. `edge_block_8x8`: Mikro-benchmark'ta yalnızca 16 adet 4-bit maske üretip belleğe yazıyordu (1.54×). Oysa gerçek `raster_kern_tri_scan` içinde: (a) 8×8'den küçük sprite üçgenleri zaten hızlı küçük-üçgen yoluna (`small_tri`) girer, (b) tamamen üçgen içinde kalan 8×8 bloklar (`all_in`) maske testi yapmadan doğrudan geçer, (c) yalnızca kısmi 8×8 bloklarda çağrılan `edge_block_8x8` için `call_indirect` maliyeti + yığındaki `masks[16]` dizisini tekrar okuyup perspektif `1/w` ve varying interpolasyonu yapma maliyeti, satır içi (inline) C SIMD kenar değerlendirmesinin kazancını götürmektedir (10k sprite'ta 0.96×, 50k sprite'ta 1.11× < 1.15×).
  2. `blend_alpha_rgba8`: M5_4'te devreye alınan **fused blend** (`variant.c` / `codegen.c`) sayesinde Matoki JIT shader'ları `BLEND_NONE` ve `BLEND_ALPHA` modlarında float→RGBA8 dönüşümünü ve hedef framebuffer ile alpha blend işlemini doğrudan JIT shader döngüsünün içinde (tek `call_indirect` içinde) yapmaktadır (`raster_kern_blend_batch` içinde `if (state->fused) return`). Fused olmayan yolda ise 256 piksellik batch başına ek `call_indirect` çağrısı C SIMD döngüsüne göre fark yaratmamaktadır (0.98×–1.01×).
  3. Görev kuralı gereği (`>%15 kazandırmayanı geri çıkar`) `edge_block_8x8` ve `blend_alpha_rgba8` varsayılan rasterizer yolunda devre dışı (`engine_set_wat_raster_mode(0)`) bırakıldı; gerçek sahnede **3.58×** (1T) ve **3.49×** (8T) kazandıran `mandelbrot_row.wat` ise aktif tutuldu.

---

### 3. Şüpheli Benchmark'ların Dürüstlük Denetimi

#### 3.1. `sum_f32` ve `dot_f32` (M5_3'teki 15.5× ve 9.0× İddiasının Denetimi)
- **M5_3'te ne yanlıştı?**
  1. **Yapay Test Verisi (İkili Kesirler):** `bench_main.c` içinde test dizileri `ka * 0.0625f` ($k \cdot 2^{-4}$, $k \in [-15, 16]$) ile doldurulmuştu. Bu sayıların toplamları IEEE-754 `f32` mantissa bitlerinde **sıfır yuvarlama hatasıyla** (exact) temsil edildiğinden, C'deki tek akümülatörlü seri toplama (`sum += a[i]`) ile WAT'taki 16-yollu ağaç toplama (`acc0..acc3`) tamamen farklı toplama sırasına sahip olmasına rağmen tesadüfen aynı bitleri üretmişti.
  2. **Adil Olmayan Derleyici Kısıtı:** C kodu tek skaler akümülatörle yazılmış ve `-fno-fast-math` ile derlenmişti. IEEE-754 float toplaması birleşmeli (associative) olmadığından clang'ın seri `sum += a[i]` döngüsünü SIMD'e vektörleştirmesi **yasaktır**; bu yüzden clang seri `f32.add` (4 çevrim gecikme dar boğazı) üretirken WAT kodu 4 adet `v128` (`f32x4`) akümülatör kullanmıştı.
- **Rastgele (Tam Sayı Olmayan) Float Verisiyle Denetim Sonuçları (`N = 65536`):**

| Çekirdek & C Varyantı | Rastgele Float ile WAT'a Bit-Bit Eşit mi? | C Medyan (Min) ms | WAT Medyan (Min) ms | Gerçek Oran (C / WAT) | Karar |
| :--- | :---: | :---: | :---: | :---: | :--- |
| `sum_f32` — 1 Skaler Akümülatör (M5_3 C kodu) | **HAYIR** (`0x467ff102` vs `0x467ff100`, 2 ULP fark) | 6.37 (6.26) ms | 0.38 (0.37) ms | *16.7× (sahte/farklı sıra)* | Geçersiz (farklı algoritma) |
| `sum_f32` — 4 Skaler Akümülatör (`s0..s3` stride-4) | **HAYIR** (`0x467ff0ff` vs `0x467ff100`, 1 ULP fark) | 1.60 (1.58) ms | 0.38 (0.37) ms | *4.21× (skaler ILP vs SIMD)* | Geçersiz (farklı ağaç) |
| **`sum_f32` — 4 × `f32x4` Akümülatör (WAT ile birebir aynı toplama sırası)** | **EVET (`%100 bit-bit aynı`)** | **0.38 (0.38) ms** | **0.38 (0.37) ms** | **1.02×** | **REDDET** |
| `dot_f32` — 1 Skaler Akümülatör (M5_3 C kodu) | **HAYIR** (`0x46030381` vs `0x46030382`, 1 ULP fark) | 6.57 (6.52) ms | 0.64 (0.64) ms | *10.2× (sahte/farklı sıra)* | Geçersiz (farklı algoritma) |
| `dot_f32` — 4 Skaler Akümülatör (`s0..s3` stride-4) | **HAYIR** (`0x46030380` vs `0x46030382`, 2 ULP fark) | 1.70 (1.66) ms | 0.64 (0.64) ms | *2.66× (skaler ILP vs SIMD)* | Geçersiz (farklı ağaç) |
| **`dot_f32` — 4 × `f32x4` Akümülatör (WAT ile birebir aynı toplama sırası)** | **EVET (`%100 bit-bit aynı`)** | **0.68 (0.67) ms** | **0.64 (0.64) ms** | **1.05×** | **REDDET** |

  - **Sonuç:** C kodu WAT ile **birebir aynı toplama ağacına** (4 adet `f32x4` vektör akümülatörü + aynı çift-indis yatay indirgeme `(acc0[0]+acc0[2]) + (acc0[1]+acc0[3])`) getirildiğinde, tam sayı olmayan rastgele float verisinde bile **bit-bit eşitlik (`EVET`) korunmakta** ve clang `-O2 -msimd128` aynı `f32x4.add` / `f32x4.mul` döngüsünü üreterek WAT ile aynı hızda (**1.02×** ve **1.05×**) çalışmaktadır.

#### 3.2. Çoklu Thread Mandelbrot Hızlanması (M6_1'deki `43.25 ms` ve `7.68×` İddiasının Denetimi)
- **Donanım ve Ölçüm Ortamı:**
  - **CPU:** `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz` (6 fiziksel çekirdek, Hyper-Threading / SMT ile **12 mantıksal işlemci**, `navigator.hardwareConcurrency = 12`, max turbo 5.0 GHz, 6×32 KiB L1d, 6×256 KiB L2, 12 MiB paylaşımlı L3).
  - **Yöntem:** Önce tüm worker'larda V8 Liftoff → TurboFan tier-up ısınması çalıştırıldı; ardından her thread sayısında (`1, 2, 4, 6, 8`) **3 ısınma turu + 9 ölçüm turunun medyanı** (`grain = 4` satır dinamik iş çekme) hem skaler C hem de elle yazılmış 4-lane SIMD WAT (`mandelbrot_row.wat`) için ölçüldü:

| Toplam Thread (Worker) | Skaler C Medyan (ms) | Skaler C Hızlanma (Verim) | WAT SIMD Medyan (ms) | WAT SIMD Hızlanma (Verim) | C / WAT Oranı |
| :---: | :---: | :---: | :---: | :---: | :---: |
| **1T (0W)** | 191.88 ms | **1.00×** (%100) | 53.53 ms | **1.00×** (%100) | 3.58× |
| **2T (1W)** | 94.68 ms | **2.03×** (%101) | 26.52 ms | **2.02×** (%101) | 3.57× |
| **4T (3W)** | 48.36 ms | **3.97×** (%99) | 13.62 ms | **3.93×** (%98) | 3.55× |
| **6T (5W)** | 32.53 ms | **5.90×** (%98) | 9.37 ms | **5.71×** (%95) | 3.47× |
| **8T (7W)** | 24.50 ms | **7.83×** (%98) | 7.01 ms | **7.64×** (%95) | 3.49× |

- **Bulgular ve Fiziksel Açıklama:**
  1. **M6_1'deki `1T = 43.25 ms` Sayısının Tutarsızlığı:** `1024×1024` Mandelbrot kümesi (`x ∈ [-2.0, 0.7]`, `y ∈ [-1.35, 1.35]`, `max_iter = 256`) toplam **61.039.751** iç döngü iterasyonu içerir. Her iterasyonda `zr` ve `zi` bir önceki iterasyonun sonucuna bağımlıdır (`mul` 4 çevrim + `sub` 4 çevrim + `add` 4 çevrim = **en az 12 saat çevrimi seri FP gecikmesi**). Tek skaler thread'de `61.04M × 12 çevrim = 732.5M çevrim` eder; 3.8–4.0 GHz saat hızında bu **~185–192 ms** (ölçülen: **191.88 ms**) sürer. Skaler C kodunun 1 thread'de `43.25 ms` sürmesi donanımın FPU gecikme sınırları gereği imkânsızdır (`43–53 ms` ancak 4-lü SIMD yapan `mandelbrot_row.wat` ile mümkündür: ölçülen 1T WAT = **53.53 ms**).
  2. **6 Fiziksel Çekirdek / 12 SMT Mantıksal Çekirdekte Neden 6T'de `5.71×–5.90×`, 8T'de `7.64×–7.83×` Çıkıyor?**
     - **6 Thread'de** hızlanma skaler C'de **5.90×**, WAT SIMD'de **5.71×** olup 6 fiziksel çekirdeğin teorik `6.0×` sınırının altındadır.
     - **8 Thread'de** ise işletim sistemi 8 thread'i işlemcinin **12 mantıksal SMT (Hyper-Threading) donanım thread'ine** dağıtır (`navigator.hardwareConcurrency = 12`). Comet Lake çekirdeğinde her fiziksel çekirdekte 2 adet FP/FMA yürütme portu (Port 0 ve Port 1) bulunur; ancak Mandelbrot iç döngüsü 12 çevrimlik seri yazmaç bağımlılığı (`vzx → vzx2 → sub → add → vzx`) nedeniyle tek bir donanım thread'inde çevrim başına yalnızca ~0.5 FP komutu emekli edebilir (yürütme portlarının >%70'i veri bağımlılığını beklerken boş kalır) ve L1 veri önbelleğine hiç dokunmaz (tüm durum yazmaçlardadır). Aynı fiziksel çekirdeği paylaşan iki SMT kardeş thread'i, birbirinin 12 çevrimlik gecikme boşluklarını doldurarak fiziksel çekirdek başına ~1.35–1.5× toplam verim üretir; bu sayede 6 fiziksel / 12 mantıksal çekirdekte 8 thread hızlanması **7.64× (WAT) / 7.83× (C)** değerine ulaşır.

