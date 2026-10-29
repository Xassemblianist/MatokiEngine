# ARCH_V2 Kırmızı Takım İncelemesi

Durum: İNCELEME (yalnız okuma; koda dokunulmadı) · 2026-10-29 · Görev: `tasks/F0_F_arch_redteam.md` · Dal: `F0_F_arch_redteam`
İncelenen: `docs/ARCH_V2.md` (109 satır), `docs/DECISIONS.md` (D-105..D-109 + ilgili eski kararlar), `docs/ARCHITECTURE.md`,
`AGENTS.md`, `docs/MATOKI.md`, `tasks/F0_*.md`, `tasks/M15_*.md`, `tasks/M16_*.md` ve kod tabanı (aşağıda her iddia için `dosya:satır`).

## 0. Okuma notları

- **Kanıt kuralı:** Koda dair her iddia açılıp okunmuş dosyaya `dosya:satır` ile bağlıdır. Var olduğunu doğrulamadığım
  dosya/özellikten bahsetmem. `src/gpu/`, `src/graph/`, `src/sim/`, `src/spec/`, `web/gpu.js`, `web/caps.js` **bugün yok**
  (`ls` ile doğrulandı) — yani ARCH_V2'nin L1–L4'ünün büyük kısmı henüz yalnız kâğıtta.
- **Web platformu iddiaları:** Emin olmadıklarım `⚠ doğrulanmalı` ile işaretli ve §10'da toplu listelendi (F0_E bunları kapatmalı).
- **Öncelik:** **P0** = Faz 1 başlamadan çözülmezse mimariyi geri dönüşsüz bozar · **P1** = ilgili faz başlamadan ·
  **P2** = tasarım sırasında · **P3** = not.
- Kıyas olarak anılan Unreal / Frostbite / id Tech / Jolt / Bevy bilgileri genel bilinen mimari düzeyindedir; sürüm ayrıntısı
  gereken yerde `⚠ doğrulanmalı` yazdım.

## 1. Yönetici özeti

ARCH_V2'nin **yönü doğru, sırası ve iddia genişliği riskli.** Güçlü yanlar: her ilke ölçülebilir bir hipoteze bağlı (§7),
"tavanı hedefle + tespit + yedek" (D-109) doğru çerçeve, ikili komut akışı (D-108) hem JS'i ince tutar hem de ücretsiz bir
yakalama/oynatma (capture/replay) formatı verir, ve wasm'ın non-relaxed f32/SIMD'i gerçekten belirlenimcidir — bu, native
motorların çoğunun sahip olmadığı bir **rekabet avantajı** (yalnız ona zarar vermezsek).

Kırmızı bulgular (ayrıntı §3–§9):

1. **Belirlenimcilik değişmezi (§6) bugün kodda hiçbir yerde sağlanmıyor ve D-109 ile kendi içinde çelişiyor**: relaxed SIMD
   (`D-036` yasak ↔ `D-109`/`M16_B` zorunlu), çoklu wasm derlemesi (adres/yerleşim farkı), CPU↔GPU'da ölçülen maliyetle iş
   seçimi ve "işaretçisiz durum bölgesi" (ECS/Fizik/Arena işaretçi dolu) birlikte yaşayamaz.
2. **"Tek hesap dili" (Matoki IR) iddiası bir IR yeniden yazımıdır**, uzantı değil: bugünkü IR skaler, tek fonksiyonlu, 2×2 quad
   piksel IR'ıdır; bellek, döngü, atomik, struct, buffer yok (`ir.h:2-3`, `MATOKI.md:120-124`).
3. **Çalışma anı özelleştirme (ilke 3) için JIT altyapısı yaşam döngüsü yok**: modül/tablo sızıntısı, 1024 slot tavanı, worker
   başına eşzamanlı derleme, ana iş parçacığında ack spin'i (en çok 2 s), worker'ların JS'e ancak ≥2 ms'de dönmesi.
4. **Fizik ARCH_V2'de neredeyse hiç yok** (yalnız §6'da "sim belirlenimci"); koddaki fizik karakter-sınıfı (küre/kapsül, dönme yok,
   tek temas noktası, O(n²), tek iş parçacığı, SIMD yok). "Birinci sınıf, belirlenimci, SIMD, iş parçacıklı fizik" sıfırdan
   yeni bir alt sistemdir (§4).
5. **Faz sırası ters**: GPU render (Faz 1) belirlenimcilik/durum bölgesinden (her sistemin yerleşimini belirleyen) önce geliyor;
   her yeni sistem işaretçili durumla yazıldıkça geri dönüş maliyeti büyüyor (§8).

## 2. ARCH_V2 ↔ kod tabanı gerçeği (neyi yeniden yazmak gerekir)

| ARCH_V2 öğesi | Repoda bugün (kanıt) | Karar |
|---|---|---|
| L0 bellek bölgeleri (§2) | 3 sabit arena: perm 64 MB, frame 128 MB, asset 48 MB (`src/app/main.c:279-281`); `Arena` mutlak `u8* base` tutar (`src/core/arena.h:10-14`); durum `static` global struct'larda (`src/app/scene_ocak.c:112` `static Ocak g_o;`) | **Yeni + yeniden yaz** (durum bölgesi, ofset tabanlı arena, halka, havuz) |
| L0 iş çalan zamanlayıcı (§2) | Tek global MPMC FIFO, kapasite 1024 (`src/thread/job.c:28`, `job.h:13`), öncelik/bağımlılık yok | **Yeniden yaz** |
| L0 RHI + render worker (§2) | `src/gpu/` yok; `engine_frame` ana iş parçacığında (`web/platform.js:339`) | **Yeni** (M15_B) |
| L0 yetenek raporu (§2) | `web/caps.js` yok; tek derleme, atomics zorunlu (`Makefile:18,22`) | **Yeni** (M16_B) |
| L1 Matoki IR (§3) | Piksel IR: skaler SSA, tek fonksiyon, 4 lane (`src/matoki/ir.h:2-3`) | **Yeniden tasarla** (`compute`/`system` için) |
| L1 WGSL codegen (§3) | Yok (`src/matoki/` altında wgsl.c yok); M15_A `TODO` | **Yeni** |
| L1 çalışma anı özelleştirme (§3) | Varyant önbelleği 64 giriş (`src/matoki/variant.h:19`) + JIT köprüsü | **Genişlet + yaşam döngüsü ekle** |
| L2 ECS arketip SoA (§4) | **Sparse-set** ECS (`src/scene/ecs.h:1-5`), yalnız `scene_ocak.c` kullanıyor | **Yeniden yaz** |
| L2 GPU sahnesi / sanal varlık (§4) | Yok | **Yeni** |
| L3 kare grafiği (§5) | `src/graph/` yok; fork-join `job_parallel_for` (`job.c:190-241`) | **Yeni** (F0_C) |
| L4 render v2 (§8 Faz 3) | CPU raster + ertelenmiş ön uç (D-105; `src/gfx3d/pipeline.c`) | **Yeni** |
| L4 fizik (§1 L4) | Karakter-sınıfı; dinamik gövde yalnız küre/kapsül (`src/phys/world.h:22-25`) | **Büyük ölçüde yeni** (§4) |
| L4 rollback ağ (§1 L4) | `src/sim/` yok; M16_A `TODO` | **Yeni** |

**Yeniden kullanılabilir iyi temeller:** D-053 deseni ("karo = iş birimi, ayrık yazım, 1T = 8T bit-bit", `DECISIONS.md:198-201`);
D-105 ertelenmiş komut kaydı (RHI komut akışının öncülü); `arena_sub` ile iş parçacığı başına dilim (`pipeline.c:534-542`);
`scene_io` (alan betimleyicili serileştirici, `src/scene/scene_io.h:1-3`) — işaretçisiz anlık görüntü için yedek yol;
`masm/mbin/jit` zinciri; 28 test dosyası + `ir_eval` bit-bit kâhin kültürü (D-032).

---

## 3. Bölüm bölüm eleştiri

Biçim: **[ID] Başlık** — *Öncelik* · ARCH_V2 bağı → Bulgu · Kanıt · **Öneri**.

### 3.1 §0 İlkeler ve §6 Değişmezler — belirlenimcilik

**[A-01] D-036 ↔ D-109 çelişkisi: relaxed SIMD** — *P0* · §0 ilke 1 (`ARCH_V2.md:11`), §6 (`:81`), L0 platform (`:29`)
- Bulgu: `D-036` **KABUL**'lü kural "Relaxed-SIMD kullanılmaz; donanıma bağlı (x86 FMA/pshufb vs ARM) sonuçlar bit-bit kâhin
  kuralını (D-032) bozar" der (`DECISIONS.md:145-147`). `D-109` aynı şeyi varsayılan yapar (`DECISIONS.md:436-437`, `:441`),
  `M16_B` kabul kriteri "relaxed ile normal derleme tolerans içinde aynı" der (`tasks/M16_B_platform_caps.md:25`).
  D-036 hiçbir yerde `YERİNE GEÇTİ` yapılmamış; oysa `DECISIONS.md:8` "eski girdiyi silme, durumunu güncelle" der. ARCH_V2
  D-036'dan hiç bahsetmez. Relaxed `madd`'in sonucunun fused/unfused olması uygulamaya bağlıdır (`⚠ doğrulanmalı`: spec metni).
  Tolerans-içi eşitlik **rollback'te yetmez** (§6 "aynı girdi → aynı bit").
- **Öneri:** Belirlenimcilik **sınıfları** tanımla ve D-110 olarak kaydet:
  (1) *Sim-kritik* (ECS sim sistemleri, fizik, oyun mantığı, hash'e giren her şey): non-relaxed wasm, GPU yok, `-ffp-contract=off`
  açıkça verilir (bugün `Makefile`'da yok; `grep` ile doğrulandı); (2) *Yalnız-görsel* (vertex/skin/gölge kodlama): relaxed ve GPU serbest.
  Sınıf ayrımı derleme birimi düzeyinde olsun (`sim_*.c` relaxed derlemeye girmez). D-036'yı "kapsamı daraltıldı" olarak güncelle.

**[A-02] "İşaretçi yok" değişmezi bugün hiçbir yerde sağlanmıyor** — *P0* · §0 ilke 5 (`:16-17`), §2 durum bölgesi (`:40`), §6 (`:81`)
- Bulgu: ECS, fizik, BVH, arena işaretçi doludur:
  - `Arena{u8* base,...}` mutlak adres, hizalama mutlak adrese göre (`arena.h:10-14`, `arena.c:45-47`) → yerleşim taban adrese bağlı.
  - `EcsStore`: `sparse*`, `dense_ent*`, `data*`, `const char* name` (`ecs.h:68-81`); `EcsWorld`: `Arena*`, 8 işaretçi dizi, `const char** strings`
    (`ecs.h:94-130`). `ecs_store_grow` eski dizileri arenada **bırakır** (`ecs.c:147-163`) → durum bölgesi şişer, anlık görüntü büyür.
  - `PhysWorld`: `bodies*`, `statics*`, `meshes*` ve `PhysStaticMesh{const Mesh*, const Bvh*}` (`world.h:47-52`, `:62-75`); `Bvh` işaretçi doludur (`bvh.h:28-35`).
  - Durum bölgede değil `.bss`'te: `static Ocak g_o;` içinde ECS + fizik + dokular + render kaynakları iç içe (`scene_ocak.c:112`, `:238`);
    `g_perm_arena` vb. global (`main.c:77-78`).
  - Aynı adresli bir bölgeyi `memcpy` ile geri yüklemek tek bir örnekte "çalışır" gibi görünür ama (a) çapraz-eş (peer) hash'i
    adresleri içerir (`const char* name` rodata adresi, derlemeler arası farklı), (b) çoklu derleme (`engine.wasm` + `engine_relaxed.wasm`, D-109)
    adres düzenini değiştirir → farklı derlemelerdeki iki eş aynı durumu aynı bitlerle **temsil edemez**.
- **Öneri:** Önce **durum bölgesi prototipi** (§9 madde 3): ofset tabanlı `RegionArena` (taban adres saklamaz), `StateRef`/tutamaç tipleri,
  durum başlıklarında `*` yasağı için `make check` denetimi (`state_*.h` öneki + `_Static_assert`), sim/render ayrımı (render kaynakları durumdan
  ayrı yapıya). Ara çözüm: aynı derleme + aynı taban adres şartıyla `memcpy` anlık görüntü, çapraz-eş hash'i `scene_io`
  benzeri alan-betimleyicili yoldan.

**[A-03] NaN, FMA daralması, `m_min/m_max`** — *P1* · §6 (`:81`)
- Bulgu: wasm'ın non-relaxed f32 `add/mul/div/sqrt` işlemleri IEEE'dir; **NaN yük (payload) bitleri belirsizdir** (`⚠ doğrulanmalı`: wasm
  nondeterminism notları) — durum hash'ine NaN girerse eşler ayrışabilir. `m_min/m_max` `<`/`>` ile yazılmış (`core/math.h:42-43`), NaN davranışı
  `f32.min/max`'tan farklı; `m_sin/m_cos` yalnız f32 mul/add polinomu (`core/math.c:36-74`) — belirlenimci (iyi), fakat yalnız FMA daralması
  olmadığı sürece.
- **Öneri:** Sim'de NaN'ı yasakla (debug'da assert, release'te hash öncesi kanonikleştir); `-ffp-contract=off`'u sim birimlerine açıkça yaz;
  `m_*` fonksiyonlarına "belirlenimcilik sözleşmesi" yorumu ekle ve test et.

**[A-04] Sim zamana bağlı: sabit adım yok, f32 biriktirici** — *P0* · §6 (`:81`), §2 kare halkası (`:41`)
- Bulgu: `engine_frame(now_ms, dt)` duvar saati `dt` alır ve sahnelere `step_dt` olarak verir (`main.c:581`; `platform.js:334` dt'yi 0.1 s'ye kırpar).
  Fizik `phys_world_update(f32 dt)` f32 biriktirici kullanır; adım sınırına takılınca birikmiş zamanı **atar** (`world.c:292-305`) — yani sonuç,
  makinenin hızına/duraksamalarına bağlı. D-094 "sabit 1/120 s, tekrarlanabilir" der (`DECISIONS.md:328-330`) ama bu yalnız bir çağrı içinde doğru.
- **Öneri:** Motor düzeyinde **tamsayı tick** (`u64 tick`); sim yalnız `tick` ve o tick'in girdisini görür; duvar saati → tick dönüşümü (tamsayı
  mikrosaniye biriktirici) motorun *dışında* (L0 zamanlayıcı). `phys_world_update(dt)` sim yolundan çıkarılır.

**[A-05] "Paralellik sonucu sıraya bağlamaz" kuralı denetlenmiyor** — *P1* · §6 (`:81`)
- Bulgu: `job_parallel_for` dilimleri atomik sayaçla dinamik dağıtır (`job.c:173-188`) → hangi iş parçacığının hangi dilimi aldığı belirsiz. Render'da sorun yok
  (D-053: ayrık yazım). Ama sim'de f32 toplama/azaltma sırası, `grain`/dilim sayısının `thread_count()`'a bağlı olması (örn. `pipeline.c:534-542` dilim boyu `nth`'ye
  bağlı, render için masum) sonuçları değiştirir. Hiçbir sim modülünde "1T = 2T = 8T" testi yok (`tests/test_phys.c` testleri aşağıda §4).
- **Öneri:** Sim iş kuralları: (1) ayrık yazım, (2) azaltma = sabit boyutlu bloklar + sabit sırada ağaç (blok boyutu iş parçacığı sayısından bağımsız),
  (3) float atomik yok, (4) her sim sistemi için CI'da "1T/2T/8T hash eşit" testi. D-053'ü şablon yap.

**[A-06] Çoklu derleme + rollback** — *P1* · D-109 (`DECISIONS.md:441`)
- Bulgu: D-109 "birden çok wasm derlemesi kabul" der. İki eş farklı derleme çalıştırırsa (biri relaxed var/yok) sim-kritik kod aynı bitleri üretmek zorunda;
  yalnız [A-01] sınıflaması + [A-02] ofset tabanlı durum bunu mümkün kılar.
- **Öneri:** Eşleşme el sıkışmasında "sim ABI hash'i" (sim derlemesi özeti + sürüm) değiş-tokuş et; uyuşmazsa rollback oyununa izin verme.

### 3.2 §0 ilke 2 ve §3 L1 — "Tek hesap dili"

**[B-01] Matoki IR, `compute`/`system` için yetersiz — bu bir IR yeniden tasarımı** — *P0* · §0 ilke 2 (`:12`), §3 tablo (`:55-60`)
- Bulgu: `ir.h` "tamamen SKALER, SSA, **tek fonksiyon**; varying = 4 lane (2×2 quad)" (`ir.h:2-3`); `IR_OUTPUT` her kanalı fonksiyon sonunda bir kez yazar
  (`ir.h:70`); tek bellek erişimi `IR_TEXEL_LOAD` (`ir.h:63`); `IR_FRAG_X/Y`, `IR_DERIV_X/Y` quad'a gömülü (`ir.h:34-35`, `:73-74`). Dil: `for` yalnız derleme zamanı sabit N,
  `return` yalnız sonda, "struct, dizi, pointer, vertex shader, **compute kernel'leri** v0'da OLMAYAN" (`MATOKI.md:63,65,120-124`). Dallar maskeli (iki taraf da hesaplanır, `MATOKI.md:60-61`).
  ARCH_V2 §3'ün `compute` (buffer okuma/yazma, atomik) ve `system` (arketip döngüsü) türleri için gereken: veri-bağımlı döngü, buffer/işaretçi tipleri, atomik, workgroup belleği + bariyer,
  `u32` karşılaştırma/`i64`/`f16`, kernel başına serbest vektör genişliği, indirekt erişim (gather). Etki: `lower.c` (953), `opt.c` (1260), `codegen.c` (2126), `ir_eval.c` (571)
  satırlık zincirin hepsi (ve D-032 sıfır-tolerans kâhini) yeniden düşünülür.
- **Öneri:** Faz 0'da IR v2 kapsam kararı (§9 madde 6). İki yol: (a) **IR v2 (yapısal döngü + bellek + atomik)** — pahalı, esnek;
  (b) **kısıtlı veri-paralel şablonlar** (`map`/`reduce`/`scan`/`gather`; keyfi döngü yok) — iki arka uçta kolay doğrulanır ve belirlenimci. Faz 1'i (b) ile sınırla; (a)'yı
  yalnız F0_A+B ölçümleri gerektirirse aç. Piksel IR'ı olduğu gibi kalsın (regresyon yok).

**[B-02] H2 ölçütü ("bit/ulp toleransı") zayıf ve yanlış hedefe bakıyor** — *P1* · §0 ilke 7 (`:19`), §7 H2 (`:91`)
- Bulgu: Bugünkü kâhin **sıfır-tolerans** ve wasm kurallarını birebir uygular (D-032, `DECISIONS.md:126-130`). GPU f32'si IEEE-katı değildir (hata sınırları, FMA daralması,
  denormal davranışı: `⚠ doğrulanmalı` WGSL spec kesinlik bölümü) → D-032 GPU'ya genişletilemez; "CPU ile GPU çıktısı birbirinin referansıdır" (ilke 7) yalnız **toleranslı**
  doğrudur. Ayrıca WGSL, türev/örnekleme çağrılarının tekdüze kontrol akışında olmasını ister (`⚠ doğrulanmalı`: derivative uniformity); Matoki ise varying `if` içinde dFdx/sample'a
  izin verir ve bunu yardımcı-lane semantiğiyle tanımlar (`MATOKI.md:76-84`) → WGSL'e iniş sırasında türev/örnekleme'yi dal dışına taşımak gerekir (M15_A kapsamı).
- **Öneri:** H2'yi sınıfla: (i) tamsayı/bit işlemleri **bit-exact** zorunlu (hash, sıralama, prefix-sum `u32`); (ii) f32 aritmetik: işlem başına ulp bütçesi; (iii) transandantal: geniş tolerans.
  "3 çekirdek" yerine D-032 tarzı **rastgele program fuzz'ı** (500 program) GPU yoluna da uygulansın.

**[B-03] `system` çekirdeği = yalnız "saf eleman-bazlı SoA" alt sınıfı** — *P2* · §3 (`:60`)
- Bulgu: Maskeli SPMD (`MATOKI.md:60-61`) hareket/yerçekimi gibi dalsız eleman-bazlı sistemlerde iyi; yapısal değişiklik (spawn/despawn), varlık referansı çözme, rastgele erişim içeren
  gerçek oyun sistemleri bu modelde yazılamaz.
- **Öneri:** "system çekirdeği yalnız şu sınıf için; gerisi C kalır" diye ARCH_V2'ye yaz; sınıf dışı için derleme hatası ver (sessiz yanlış kod yok — `M15_A` kabul ruhu).

### 3.3 §0 ilke 3 ve §3 — "Motor kendini sahneye göre derler"

**[C-01] JIT altyapısının yaşam döngüsü yok; "ms mertebesi" ölçülmemiş** — *P0* · §0 ilke 3 (`:13-14`), §3 (`:62-63`)
- Bulgu (hepsi okunan kod):
  1. Her worker `new WebAssembly.Module(wasmBytes)` ile **eşzamanlı** derler (`web/jit.js:26`); ana iş parçacığı da eşzamanlı (`jit.js:49`). Modül nesnesi paylaşılmıyor, bayt dizisi yayılıyor (`jit.js:67-74`).
  2. Kayıt listesi ve tablo **yalnız büyür**: `jitRegistry.push` (`jit.js:65`), `table.grow(1)` (`jit.js:61`); serbest bırakma yok. Tavan `JIT_MAX_FUNCS = 1024` (`thread.h:19`).
  3. Varyant önbelleği dolunca başlangıç slotunun üzerine yazar (`variant.c:144-146`) ama evicted varyantın tablo slotu ve kalıcı wasm baytları (`variant.c:212`, `ARENA_PUSH_ARRAY(cache->persist…)`) serbest kalmaz → **sızıntı**.
  4. Önbellek anahtarı sabit uniform **değerlerini** içerir (`variant.h:26-36`; ARCH_V2 `:62` "sabit uniform'lar") → `dt`/`g` değişince yeni derleme: sınırsız varyant.
  5. Çoğaltma el sıkışması: ana iş parçacığı **spin** ile en çok **2000 ms** bekler (`thread.c:121-157`, `:140`); worker'lar yalnız `engine_worker_loop` döndüğünde JS mesajı görür (`job.c:278-282`, `worker.js:34-46`, D-052 `DECISIONS.md:193-196`) — yani bir worker uzun bir iş içindeyken çoğaltma o işin süresi kadar bekler.
  6. **Worker'lar JIT derleyemez**: worker'daki `plat_jit_load` sabit `-1` döner (`worker.js:76-79`) → derleme yalnız ana iş parçacığında; arka planda "özelleştirme işi" yazılamaz ve M16_B (motoru worker'a taşı) ile **doğrudan çakışır** (engine_frame worker'a gidince `plat_jit_load` orada da stub).
  7. Bugünkü kullanım: shader başına 1 + 3 füzyon modülü (`app/shaders.c:367`, `:387-397`), ölçülen `compile_ms` ack beklemeyi de içerir (`shaders.c:367-369`). ARCH_V2'deki "arketip/mesh/materyal başına yeniden derleme" bunun ≥ 10–100 katı modül üretir.
- **Öneri:**
  - Anahtara yalnız **yapısal** girdiler (arketip düzeni, tipler, materyal şeması) gir; sabit değerler çalışma anı uniform'u kalsın; yalnız ömür boyu sabit olduğu kanıtlananlar gömülsün.
  - Kod ömrü yönetimi: modül havuzu + nesil + tablo slot serbest listesi + `JIT_MAX_FUNCS` yerine büyüyebilir ack yapısı; evict ⇒ tablo slotu geri verilir.
  - **Kademeli (tiered) çalışma:** genel yol çalışmaya devam eder, özelleştirilmiş modül `WebAssembly.compile` (asenkron) ile hazır olunca tek kare sınırında takas edilir; asla kare donması yok.
  - Derlenmiş `WebAssembly.Module`'ü worker'lara baytla değil modül nesnesi olarak yaymak (`postMessage` ile klonlama; motorun derlenmiş kodu paylaşıp paylaşmadığı `⚠ doğrulanmalı`).
  - Çoğaltma kanalını JS olay döngüsüne bağlı bırakma: worker'da `Atomics.waitAsync` (D-109 zaten listeliyor; worker'da desteği `⚠ doğrulanmalı`) ile futex + mesaj birleşsin; 2 ms zaman aşımı hilesi (`job.c:278`) kalksın.
  - `plat_jit_load`'ı worker'da gerçekleyin ya da derleme servisini "çağıran iş parçacığından ana iş parçacığına istek halkası" olarak tanımlayın (M16_B ile birlikte tasarla).

**[C-02] Ana iş parçacığında eşzamanlı `WebAssembly.Module` boyut sınırı** — *P1* · §3 (`:62-63`)
- Bulgu: Chrome'da ana iş parçacığında büyük (yaklaşık 4 KB üstü) modülün eşzamanlı derlenmesine sınır konduğunu hatırlıyorum (`⚠ doğrulanmalı`, güncel durum). `jit.js:49` eşzamanlı yol. Üç sistemli bir ECS çekirdeği küçük shader'dan kolayca büyük çıkar (derleme tamponu 64 KB, `shaders.c:345`).
- **Öneri:** [C-01] kademeli asenkron yol zaten çözer; yoksa F0_B ilk sıralı işi bu sınırı ölçsün.

**[C-03] H1 ölçütü yanlış karşılaştırma tabanı kullanıyor** — *P1* · §7 H1 (`:90`), F0_B
- Bulgu: H1 "genel C döngüsünü ≥ 2× geçer" der; F0_B'de genel yol = mevcut sparse-set sorgu API'si (`ecs.h:189-200`; `ecs_query_get` her varlıkta `sparse[]` arar `:196-200`). Bu taban zaten zayıf; "özelleştirmenin" kazancı ile "SoA+SIMD'e geçişin" kazancı karışır. D-042 önceliği (elle WAT'ın çoğu gerçek sahnede ≤ %1–11, `DECISIONS.md:165-173`) 2× için temkin gerektirir.
- **Öneri:** H1'i üç kola ayır: (a) mevcut ECS API, (b) **elle yazılmış SoA + SIMD C (AOT)**, (c) JIT özelleştirilmiş. Ölçüt: **(c)/(b) ≥ 1.3×** + derleme geri ödeme süresi (kaç karede amorti). (c)≈(b) ise ilke 3'ün "derleyici çalışma zamanının parçası" iddiası çöker ve L1'in yarısı AOT'ye iner (bu da geçerli bir sonuçtur).

### 3.4 §1 Katmanlar

**[D-01] L3 "kare grafiği" her şeyin tek yolu olamaz** — *P2* · §1 kural (`:31`)
- Bulgu: "L4 doğrudan RHI'ye/job'a gitmez; L3 düğümü olarak gider". Ses (AudioWorklet), girdi, ağ, varlık akışı (IO), kod derleme **kare-dışı, asenkron** servislerdir; kare grafiğine zorlamak yanlış soyutlama (Unreal'de gameplay doğrudan task graph/async kullanır; Bevy'de async task havuzu ayrıdır).
- **Öneri:** L3'ü ikiye böl: (1) **deterministik kare grafiği** (sim + render kaydı), (2) **servisler** (kuyruk/aktör: IO, ağ, ses, derleme). Servislerin sim'e girişi yalnız kare-damgalı olaylarla.

**[D-02] Eksik katman: Extract / render proxy** — *P1* · §5 (`:76`), §2 kare halkası (`:41`), §4 GPU sahnesi (`:68-69`)
- Bulgu: "sim N+1 ∥ render kaydı N" derken render, durum bölgesini okurken sim aynı bölgeyi yazar; bölge **tek** (`:40`), halka yalnız geçiciler için. Ya durum çift tamponlanmalı ya da render'ın ihtiyacı kopyalanmalı. ARCH_V2 hiçbirini tanımlamıyor (Unreal: sahne proxy'leri + render thread; Bevy: `Extract` + ayrı render world).
- **Öneri:** Açık bir **Extract düğümü**: sim durumundan yalnız render'ın ihtiyacını (dönüşüm, sınır, materyal kimliği, kirli bayrağı) kare halkasına kopyalar; H5'in "delta"sı = Extract çıktısı. Sabit-adım sim ↔ değişken-hız render için iki dönüşüm anlık görüntüsü ile enterpolasyon da burada olur. F0_C ve H3/H5 bunu ölçsün.

### 3.5 §2 L0 — Çalışma zamanı

**[E-01] Bellek tavanı ve bütçe** — *P0* · §2 (`:35-43`)
- Bulgu: ARCH_V2 "wasm32 → 4 GB tavan" der; **gerçek** bağlama 256 MB başlangıç / 1 GB tavan (`Makefile:23`; `web/platform.js:44,78` 4096/16384 sayfa; D-003 `DECISIONS.md:25-30`). Bugünkü bölgeler 240 MB sabit (`main.c:279-281`) — ve D-002 metni hâlâ "frame 16 MB" yazıyor (`DECISIONS.md:20-23`; kod 128 MB). 4 GB paylaşımlı bellek mobil/Safari'de ve `memory.grow` altında (tüm worker'larda görünüm yenileme; `platform.js:174-192` yalnız ana iş parçacığı için `refreshViews`) risklidir (`⚠ doğrulanmalı`). Anlık görüntü halkası N×durum boyutudur ve hiçbir bütçe tablosunda yok.
- **Öneri:** ARCH_V2'ye **bellek bütçesi tablosu** (hedef cihaz sınıfları: mobil ~512 MB, masaüstü ~2 GB) + durum bölgesi **üst sınırı** (rollback oyunları için hedef ≤ 1–2 MB sim durumu; büyük salt-okunur veri — BVH, mesh — durum dışında, kimlikle başvurulur). `memory64` yalnız ölçülmeye devam etsin; i64 işaretçi maliyeti `⚠ doğrulanmalı`.

**[E-02] H4 eşiği ("16 MB < 2 ms") rollback maliyetini yanlış çerçeveliyor** — *P1* · §7 H4 (`:93`), F0_D
- Bulgu: Rollback maliyeti = her kare anlık görüntü + **K kare yeniden simülasyon**; baskın terim sim adımının maliyeti ×K, `memcpy` değil. 16 MB × 60 Hz ≈ ~1 GB/s sürekli bellek trafiği ve 8–16 halka ⇒ 128–256 MB. Bekleyen öneri Ö-1: paylaşımlı bellekte `memory.fill` Firefox'ta 2× yavaş (`DECISIONS.md:447-448`) — `memory.copy` için aynı riski her tarayıcıda ölçmek gerek (F0_D kısmen kapsıyor).
- **Öneri:** H4'ü yeniden yaz: **"örnek sahnede (örn. 2 000 dinamik cisim + 10 000 ECS varlığı) 8 kare geri sarma + yeniden simülasyon ≤ 12 ms (p99)"**. Aritmetik kural: K=8 için `sim_adım_ms ≤ 16.6 / 9 ≈ 1.8 ms`. Anlık görüntüyü küçük tut (sim durumu ≤ 1–2 MB), yazma-izli/sayfa-delta anlık görüntü seçeneğini değerlendir (wasm'da sayfa koruması yok → yazılımsal kirli bitmap), hash'i artımlı yap.

**[E-03] İş zamanlayıcı: ARCH_V2 tarifi ≠ mevcut** — *P1* · §2 (`:45-47`), F0_C
- Bulgu: Tarif: "iş çalan deque'ler, öncelik sınıfları, kilitsiz kuyruklar". Gerçek: **tek global FIFO**, kapasite 1024 (`job.c:28`, `job.h:13`); öncelik yok; `job_wait` kuyruktaki **herhangi** işi çalıştırır (`job.c:147-156`) → kare-kritik bekleme sırasında akış/arka plan işi çekip gecikebilir, iç içe çağrı 1 MB iş parçacığı yığınına biner (`thread.h:15`); kuyruk dolunca `job_submit` çağıranın içinde iş çalıştırır (`job.c:135-140`) — özyineleme/derinlik riski. Boşta worker 256 dönüşlük spin + 2 ms futex + JS'e dönüş döngüsü (`job.c:254-282`; `worker.js:34-46` `setTimeout(…,0)`) → uyanma gecikmesi ve enerji; ardışık zamanlayıcı kıstırması (`⚠ doğrulanmalı`: HTML iç içe `setTimeout` ≥ 4 ms) p99'u bozabilir. Üst sınır 8 worker (`thread.h:13`).
- **Öneri:** v2: iş parçacığı başına Chase-Lev deque + global injector; öncelik sınıfı = ayrı kuyruk/havuz; `job_wait` yalnız aynı sınıf/grubu yardımcı çalıştırır; `JobCounter` → devam (continuation) bağımlılıkları (F0_C'nin bağımlılık sayaçları); worker'da futex + JS mesajı için `Atomics.waitAsync` ([C-01]).

**[E-04] "Ana iş parçacığı yalnız DOM köprüsü" hedefi ↔ fiili durum ↔ M16_B** — *P1* · §2 (`:45`)
- Bulgu: `engine_frame` ana iş parçacığında, `putImageData` ile (`platform.js:339`, `:349-352`). M16_B motoru worker'a taşır ama: (i) girdi alanları **atomik olmayan `+=`** ile yazılıyor (`platform.js:282-283`, `:297-298`) — motor ayrı iş parçacığında okuyup sıfırlarken kayıp güncelleme; (ii) girdi **durum** olarak tutuluyor (D-005), kare-damgalı **olay** değil — rollback girdi halkası (M16_A) olay ister; (iii) JIT [C-01/6] stub'ı.
- **Öneri:** Girdi için SPSC olay halkası (kare/tick damgalı), pointer-lock/DOM işleri ana iş parçacığında kalsın; JIT servisini M16_B ile birlikte tasarla.

**[E-05] "Taban yalnız çalışır" yedeği bugün yok** — *P1* · D-109 (`DECISIONS.md:438-440`)
- Bulgu: Tek derleme `-matomics` + `--shared-memory` (`Makefile:18,22`). SAB yoksa (COOP/COEP başlığı verilemeyen üçüncü taraf gömme/portal ortamları: `⚠ doğrulanmalı`) `platform.js:76-112` önce `shared:false` dener (modül paylaşımlı bellek istediğinden başarısız olur), sonra `shared:true` dener, o da olmazsa panik. Yani yedek basamak (tek iş parçacıklı derleme) yok; `tools/serve.py` başlıkları yalnız geliştirme sunucusunda (`serve.py:24-28`). `platform.js:150-152` thread başlatma hatalarını sessizce yutar.
- **Öneri:** Derleme matrisi: `engine_st.wasm` (atomics yok), `engine_mt.wasm`; yükleyici seçer; hosting rehberi (COOP/COEP, `COEP: credentialless` seçeneği `⚠ doğrulanmalı`). CI'da ST yolu çalıştırılsın.

**[E-06] Worker sayısı formülü hatası ve aşırı abonelik** — *P2* · §2 (`:46`)
- Bulgu: `Math.min(Math.max(hc - 1, 7), 8)` (`platform.js:139`) her zaman 7–8 döner; 2–4 çekirdekli/mobil cihazda aşırı abone; `job_wait` saf spin (`job.c:151-155`). `threads.js:46`'daki varsayılan formül (min 1) farklı → büyük olasılıkla `max/min` karışması.
- **Öneri:** `clamp(hc-1, 1, 8)`; mobilde 2–3; ölçülmüş throughput'a göre uyarlanır; küçük bir görev olarak açılmalı (bu inceleme dokunmaz).

**[E-07] 3 karelik boru hattı gecikmesi ↔ rollback hedefi** — *P2* · §2 (`:41`), §5 (`:76`)
- Bulgu: Sim N+1 ∥ render N ∥ GPU N−1 → ~3 kare ≈ 50 ms @60 Hz girdi→görüntü, tarayıcı birleştiricisi (compositor) gecikmesi hariç (`⚠ doğrulanmalı`). Rollback netcode'un amacı gecikmeyi düşürmek (§1 L4).
- **Öneri:** Boru hattı derinliği yapılandırılabilir (rekabetçi mod = 1–2); H11 "girdi→foton gecikmesi" ölçümü ekle (§7).

**[E-08] Akış önbelleği için ayırıcı yok** — *P2* · §2 (`:42`), §6 (`:83`)
- Bulgu: Tek ayırıcı bump arena (`arena.h:10-24`); `arena_push_raw` iş parçacığı güvenli değil (atomik yok, `arena.c:43-58`; çözüm `arena_sub` dilimleri). LRU sanal-sayfa havuzu, slab/buddy ve havuz ayırıcısı **yok**. ECS spawn/despawn'in iş içinden yapılması için komut tamponu yalnız tek iş parçacıklı (`ecs.h:120-124`).
- **Öneri:** `core/pool` (sabit boyutlu sayfa/slab, serbest liste) — ofset tabanlı; durum bölgesinin ayırıcısı ile aynı tasarım.

**[E-09] SIMD beklentisi** — *P3* · §0 ilke 3, §3
- Bulgu: wasm 128-bit (4×f32); 256-bit yok. Çözücü/gather ağır işlerde v128'de gather yok → 4× teorik, gerçekte 1.5–3× beklenmeli. Optimizasyon iddiası ölçüm gerektirir (AGENTS.md "Optimizasyon iddiası = benchmark").

### 3.6 §4 L2 — Veri

**[F-01] ECS: sparse-set → arketip geçişi gerekçesiz ve büyük** — *P0* · §4 (`:67`)
- Bulgu: Bugünkü ECS sparse-set (`ecs.h:1-5`); arketip SoA'ya geçiş tam yeniden yazımdır. Gerekçe verilmemiş: arketip depolarında yapısal değişiklik (bileşen ekle/sil) satır taşıma maliyeti yüksektir; durum etiketleri sık değişen oyunlarda arketip kıpırtısı (churn) oluşur (Bevy hem tablo hem sparse-set depolama sunar; Unity DOTS chunk modeli; Flecs benzer). Mevcut sınırlar: `ECS_MAX_COMPS = 32`, maske `u32` (`ecs.h:23`, `:103`); 12 bit nesil (`ecs.h:16-19`).
- **Öneri:** **Hibrit**: sıcak veri tablo (arketip chunk, 16 KB hizalı, render proxy ile aynı yerleşim), sık değişen etiketler sparse-set. Maskeyi 64/128 bit'e genişlet. Varlık kimlikleri eşler arası aynı olmalı → spawn/serbest-liste sırası belirlenimci kalsın (bugünkü LIFO `free_list`, `ecs.c:179-195`, zaten belirlenimci — korunmalı).

**[F-02] Sistem zamanlaması erişim bildirimine dayanmalı** — *P1* · §4 (`:67`), §5 (`:74`)
- Bulgu: `ecs_query` tek iş parçacıklı imleç (`ecs.h:132-139`); ARCH_V2 sistemlerin kaynak okuma/yazma bildirimini ve bunlardan otomatik düğüm bağımlılığını söylemiyor (Bevy'nin otomatik paralelliği buna dayanır).
- **Öneri:** `system = kare grafiği düğümü {okur: bileşenler, yazar: bileşenler}`; sıra eşitlik durumunda **bildirim sırası** (zamanlamaya değil) belirler.

**[F-03] "Veri GPU'da yaşar": aygıt kaybı (device lost) yolu yok** — *P1* · §0 ilke 6 (`:18`), §4 (`:68-69`)
- Bulgu: Sahne GPU'da kalıcı, CPU yalnız delta yollar. `GPUDevice` kaybolabilir (sekme arka planda, GPU sıfırlama, bellek baskısı: `⚠ doğrulanmalı`); delta modelinde yetkili CPU kopyası/tam yeniden eşitleme yolu tanımlı değil.
- **Öneri:** Yetkili kopya = Extract/sim durumu (CPU); GPU sahnesi **yeniden kurulabilir önbellek** olarak tasarlanır; testte `device.destroy()` ile tam resync (`⚠ doğrulanmalı`).

**[F-04] Sanal geometri/doku: WebGPU kısıtları + sıkıştırma + kapsam** — *P1* · §4 (`:70`), §8 Faz 3 (`:104`)
- Bulgu: (i) WebGPU'da donanım seyrek/sanal doku ve bindless yok (`⚠ doğrulanmalı`, F0_E) → yazılımsal dolaylı sayfa tablosu + geri besleme tamponu okuması, o da asenkron (`mapAsync`) → ≥ 1 kare gecikme/pop-in. (ii) Nanite-sınıfı compute raster 64-bit atomik ister; çekirdek WGSL atomikleri 32-bit'tir (`⚠ doğrulanmalı`). (iii) GPU doku sıkıştırma (BC/ASTC/ETC2) aygıta bağlı özelliktir (`⚠ doğrulanmalı`); repoda yalnız PNG/JPEG çözücüler var (`src/img/png.c`, `src/img/jpeg.c`) → Basis-türü transcoder sıfırdan (D-001 libc yok) gerekecek. (iv) "Visibility buffer + meshlet + compute raster + sanal gölge + GI" bir Unreal-yılları işi; ajan ekibi için kapsam fazla geniş.
- **Öneri:** Faz 3'ü ölçülmüş küçük dilimle başlat: GPU-sürümlü örnek kırpma + dolaylı çizim + kümelenmiş ileri (clustered forward); compute raster/visibility buffer yalnız F0_E sonucu 64-bit atomik/subgroup gerçeği doğrularsa.

### 3.7 §5 L3 — Kare grafiği

**[G-01] Zamanlayıcının cpu ↔ gpu_compute'u "ölçülen maliyetle" seçmesi** — *P0* · §0 ilke 4 (`:15`), §5 (`:76-77`)
- Bulgu: (i) **Belirlenimcilik ihlali**: GPU sonucu sim'e dönerse GPU bit'leri satıcıya bağlıdır ([B-02]); (ii) **geri okuma asenkron** (`mapAsync`): eşzamanlı çalışan wasm motorun ortasında bekleme noktası yok → fiilen ≥ 1 kare gecikme; (iii) maliyet ölçümü `timestamp-query` ile; bu özellik isteğe bağlı ve kaba olabilir (`⚠ doğrulanmalı`) → dinamik seçim dalgalanır (flapping); (iv) varyant matrisi patlar: {özelleştirilmiş, genel} × {cpu, gpu} × {relaxed, normal} = 8 yol, her biri için "eskisiyle karşılaştırma" testi (§6 `:84`).
- **Öneri:** Dinamik değil **statik atama** (başlangıçta ölçüm/profil + ayarlanabilir); düğümlere `sim_kritik` etiketi → yalnız CPU; GPU'ya yalnız "yalnız-görsel, tek yönlü veri akışı" işler (parçacık görseli, skinning, kırpma) gitsin; yol sayısını sınırla.

**[G-02] Bariyer/takma adlama GPU tarafında WebGPU'da sınırlı** — *P2* · §5 (`:75`)
- Bulgu: WebGPU bariyeri tarayıcı kullanım izlemesiyle otomatik yapar; kaynak belleği takma adlama (transient aliasing) API'si yok (`⚠ doğrulanmalı`). Frostbite FrameGraph / Unreal RDG'nin GPU tarafı kazancı (geçici kaynak takma adlama, async compute) burada büyük ölçüde yok.
- **Öneri:** Grafiğin GPU kısmını "geçiş sırası + kaynak havuzu (tanımlayıcıya göre yeniden kullanım) + CPU tarafı doğrulama"ya daralt; bariyer üretimini kapsam dışı yaz.

**[G-03] H3 ölçütü sentetik yük için kolay geçilir** — *P2* · §7 H3 (`:92`), F0_C
- Bulgu: "200 düğüm < 0.1 ms; boru hattı ≥ 1.3×" **sentetik** iş yüküyle ölçülür (F0_C kabul). Gerçek CPU raster yolunda 8 worker zaten tüm çekirdekleri doldurur (D-053) → sim ∥ render çekirdek bölüşümü olur, boru hattı kazancı ≈ 1.0 çıkabilir; ≥ 1.3× yalnız GPU render yolunda (CPU boşta) anlamlı.
- **Öneri:** H3'ü iki kola ayır: (a) grafik ek yükü (sentetik tamam), (b) gerçek sahne (`scene_kuyu`) kare süresi p50/p99, **CPU raster** ve **GPU** yolu ayrı.

### 3.8 §6 Değişmezler

**[H-01] "JS'te motor mantığı yok" çizgisi tanımsız** — *P2* · §6 (`:82`), D-108 (`DECISIONS.md:425-427`)
- Bulgu: RHI yürütücü fiilen **durumludur**: tutamaç→`GPUObject` tabloları, asenkron boru hattı oluşturma, hata kapsamları, `device.lost`, bind group önbelleği. `ARCHITECTURE.md:18` JS'i ~1000 satırla sınırlıyor; bugün test/bench hariç ≈ 810 satır (`platform.js` 361, `threads.js` 145, `worker.js` 123, `jit.js` 82, `audio_worklet.js` 42, `audio.js` 31, `filedrop.js` 26); `gpu.js` + `caps.js` bunu aşacak.
- **Öneri:** Çizgiyi yaz: "JS durumlu olabilir, **karar vermez**"; kaynak ömrü kararları C'de (komut akışında serbest bırakma), boru hattı hazır oluşu C'ye olay olarak döner; satır sınırını güncelle.

**[H-02] "Her yeni yol eskisiyle karşılaştırma testiyle gelir" tarayıcı gerektirenlerde insan kapılı** — *P1* · §6 (`:84`)
- Bulgu: `make check` Node/host tabanlı (`tests/test_raster_wasm.mjs`, `tools/host/`); WebGPU doğrulaması M15_B'de "yöneticiye kalabilir" (`tasks/M15_B_gpu_rhi.md` Kabul). Ölçümlerin (H2/H5) çoğu tarayıcı ister.
- **Öneri:** Headless tarayıcı CI (gece koşusu; yazılımsal WebGPU yedeği `⚠ doğrulanmalı`) + altın görüntü/hash karşılaştırması; AGENTS "npm yok" kuralı yalnız çalışma zamanı JS'i içinse, test araçları için istisna `DECISIONS.md`'ye yazılmalı (ARCH karar).

### 3.9 §7 Faz 0 hipotezleri — ölçüt yeterliliği

| # | Mevcut ölçüt | Yeterli mi? | Düzeltme |
|---|---|---|---|
| H1 | 100k varlık, 3 sistem ≥ 2× | **Hayır** — taban zayıf ([C-03]) | 3 kol; (c)/(b) ≥ 1.3×; derleme geri ödeme süresi; p95 derleme+çoğaltma gecikmesi |
| H2 | bit/ulp, 3 çekirdek | **Hayır** ([B-02]) | Sınıflı tolerans; tamsayı çekirdekler bit-exact; rastgele program fuzz |
| H3 | 200 düğüm < 0.1 ms; ≥ 1.3× | Kısmen ([G-03]) | İki kol; gerçek sahne p99 |
| H4 | 16 MB < 2 ms | **Hayır** ([E-02]) | 8 kare geri sar + yeniden simüle ≤ 12 ms p99 (gerçek fizik+ECS) |
| H5 | 100k örnek, CPU gönderimi < 0.5 ms | Belirsiz | Dinamik sahnede çoğu dönüşüm her kare kirli → delta = tam gönderim; **statik/dinamik oranı** belirtilmeli; JS komut çevirisi + `writeBuffer` dahil mi? Faz 1 sonrası değil **Faz 0'a** alınmalı (ilke 6'nın temeli) |
| H6 | tarayıcı/özellik tablosu | **Ölçüt değil** | Karar kuralı: "özellik, hedef kitlenin ≥ %X'inde varsa tavan; yoksa tespitli seçenek" + Chrome-dışı hedef (Safari/Firefox) için asgari kabul kümesi |

**Eksik hipotezler (eklenmeli):**
- **H7 Belirlenimcilik (en kritik, yok):** mevcut fizik+ECS (veya prototip) 10 000 tick; {1T, 2T, 8T} × {Chrome, Firefox, Safari} × {x86, ARM varsa} aynı hash dizisi.
- **H8 Fizik v2:** 2 000 dinamik cisim @ 120 Hz, 8T, adım ≤ 1.8 ms ve 1T = 8T hash (§4).
- **H9 JIT yaşam döngüsü:** 200 özelleştirme/oturum sonunda tablo/arena sızıntısı = 0; kare süresi p99 sıçraması < 4 ms.
- **H10 Yedek kademe:** SAB yok ⇒ ST derleme tam çalışır; CI'da koşar.
- **H11 Gecikme:** girdi→foton, boru hattı derinliği 1/2/3.
- **H12 Bütçe:** orta segment mobil cihazda 60 fps hedef senaryo + bellek tavanı.

### 3.10 §8 Fazlar ve §9 Ajan ataması

**[I-01] Faz sırası: önce durum/belirlenimcilik, sonra GPU** — *P0* · §8 (`:99-104`)
- Bulgu: Faz 1 RHI/render worker/Matoki çekirdek türlerini, durum bölgesi ve belirlenimcilik ise Faz 0'da yalnız **ölçüm** (F0_D) + Faz 2'de ECS v2 olarak koyuyor. Durum yerleşimi ([A-02]) ECS, fizik, anlık görüntü, hash, Extract ve ağı birlikte belirler; her yeni sistem işaretçili yazıldıkça geri dönüş maliyeti artar.
- **Öneri:** Faz 0'ı **durum bölgesi prototipi + H7** ile bitir; RHI/GPU işi paralel yürüsün ama sim sistemleri yeni durum kurallarıyla yazılsın.

**[I-02] Faz 0 kanıt zincirinin kendisi bağımlı** — *P2* · §7 (`:88-95`)
- Bulgu: F0_A, M15_A ve M14_B'nin merge edilmesini bekler (`tasks/F0_A_matoki_compute.md` Durum satırı); F0_E (platform taraması) D-109/relaxed/Safari kararlarını etkiliyor ama sıra son.
- **Öneri:** Bağımsız olanlar (F0_B, F0_C, F0_D, **F0_E önce**) paralel; F0_A en sona.

**[I-03] Ajan modeli darboğazı** — *P3* · §9 (`:106-109`)
- Bulgu: Tarayıcı doğrulaması yöneticiye bırakılıyor (M15_B, F0_A kabulleri) → insan kapısı ölçüm hızını belirliyor.
- **Öneri:** [H-02] headless CI kararı ile doğrulama otomatikleşsin.

**[J-01] Belge tutarlılığı** — *P2* · §0 başlık (`:6-7`), ARCHITECTURE
- `ARCHITECTURE.md:3` ("GPU kullanmadan") ve `:11` ("WebGPU/WebGL yok") D-108 sonrası bayat; ARCH_V2 `:7` hipotez kapısını "ARCHITECTURE §0.3"e atfeder, oysa §0.3 CPU-yalnız kuralıdır (`ARCHITECTURE.md:11`), ölçüm kuralı §0.4'tür (`:12`); 4 GB ↔ 1 GB ([E-01]); D-002 ↔ `main.c:280`; D-036 ↔ D-109 ([A-01]).
- **Öneri:** Tek geçişte belge temizliği (kod yok), `DECISIONS.md:8` kuralına uygun durum güncellemeleri.

---

## 4. Fizik — birinci sınıf alt sistem olarak değerlendirme (belirlenimci, SIMD, iş parçacıklı)

### 4.1 Bugünkü durum (okunan kod)

| Boyut | Bulgu | Kanıt |
|---|---|---|
| Gövde türleri | Yalnız dinamik **küre ve kapsül**; yönelim/açısal hız **yok** | `world.h:22-25`, `:27-39` |
| Temas | Çift başına **tek** temas noktası (manifold yok) | `collide.h:13-18` |
| Geniş faz | Dinamik-dinamik **O(n²)** çift döngüsü; gövde×statik doğrusal tarama | `world.c:259-260`, `:182` |
| Çözücü | Tek geçiş, gövde-indeksi sırasına bağlı Gauss-Seidel; ada (island)/uyku/ısınma yok | `world.c:235-290` |
| Adımlama | Hıza bağlı uyarlamalı alt adım (≤ 256/gövde) → maliyet sıçraması | `world.c:11-17`, `:247-255` |
| Zaman | f32 biriktirici, adım sınırında zaman atma | `world.c:292-305` |
| Bellek | İşaretçili (`bodies*`, `Mesh*`, `Bvh*`) | `world.h:47-52`, `:62-75`; `bvh.h:28-35` |
| İş parçacığı | Fizik/ECS/anim içinde **hiç** `job_*`/atomik yok (`grep` boş) | `src/phys`, `src/scene`, `src/anim` |
| SIMD | `src/phys` altında `*_wasm.c`/v128 yok | `src/phys/*` |
| Kullanım | Uygulamada yalnız `phys_move_and_slide` (karakter/düşman); dinamik `phys_world_step/update` uygulamada çağrılmıyor | `scene_kuyu.c:1042-1054,1294`, `scene_ocak.c:279` |
| Testler | 14 test: analitik temas, ışın, BVH karşılaştırma/benchmark, tünelleme; **belirlenimcilik/replay/1T=8T testi yok** | `tests/test_phys.c:41-641` |
| Belirlenimcilik zemini | `m_sin/m_cos` salt f32 polinom (iyi); `sqrt`/`floor` wasm opcode'u | `core/math.c:36-74`, `core/math.h:18-24` |

Yani bugünkü fizik **karakter denetleyicisi + statik mesh sorguları**dır; "oyun motoru fiziği" (dönen katı cisimler, kısıtlar, yığın kararlılığı) yoktur. M16_A'nın kabul ölçütü ("10 000 kare fizik + ECS replay") yalnız bu küre/kapsül dünyasını sınar (`tasks/M16_A_det_rollback.md`).

### 4.2 Önerilen hedef mimari (Jolt/Bevy dersleriyle)

1. **Durum:** gövdeler **SoA** (pos x/y/z, vel, yönelim quat, açısal hız, kütle…), durum bölgesinde ofset tabanlı; gövde kimliği (indeks, nesil). Statik geometri (BVH/mesh) salt-okunur, durum dışında, kimlikle başvurulur ([A-02]). **Temas önbelleği** (ısınma impulsları) sim durumudur: anlık görüntüye ve hash'e girer, `(a,b)` sıralı anahtarla belirlenimci saklanır.
2. **Sabit tick:** tamsayı tick, sabit `dt`, **sabit alt adım sayısı**; yüksek hız için hıza-bağlı döngü yerine spekülatif temas/CCD payı. Maliyet sınırlı ve belirlenimci ([A-04]).
3. **Geniş faz:** dinamik için ızgara ya da sırala-ve-tara (anahtar + gövde kimliğiyle **kanonik** eşitlik bozma), statik için mevcut BVH. Çift listesi `(a<b)` kanonik sırada radix sıralanır; hash-map yineleme sırasına dayanmak **yasak**.
4. **Dar faz:** dışbükey şekiller (kutu, gövde/hull, kapsül, küre, mesh), manifold ≤ 4 nokta (SAT/GJK-EPA). Önce skaler doğruluk; sonra 4'lü SoA çift-grupları için v128.
5. **Adalar:** temas/kısıt grafında union-find (kök = en küçük gövde kimliği) → ada = iş birimi; ada başına sıralı impulse çözücü (4–8 yineleme, sabit sıra). Büyük adalarda **greedy graf boyama** (kısıt-indeksi sırasıyla belirlenimci): aynı renkteki kısıtlar ayrık gövdelere dokunur ⇒ 4 kısıt/v128 ve renk içi paralel.
6. **İş parçacığı kuralları ([A-05]):** ayrık yazım; sabit boyutlu bloklar; float atomik yok; sonuç birleştirme indeks sırasında; olaylar (temas başlangıç/bitiş, tetikleyici) belirlenimci sıralı tampon. CI: **1T = 2T = 8T hash**.
7. **Özellikler:** uyku, kısıtlar/eklemler, kinematik gövde (mevcut `move_and_slide` kinematik olarak kalır), ışın/şekil taraması/çakışma sorguları (salt-okunur → serbest paralel), ragdoll (anim ile ilişki).
8. **Matematik:** yalnız non-relaxed wasm + `-ffp-contract=off` ([A-01], [A-03]); GPU/relaxed **yok**. GPU fiziği yalnız "etki" (parçacık/kumaş görseli) için.
9. **SIMD beklentisi (dürüst):** v128 = 4 lane; çözücü gather/scatter ağır (v128 gather yok). Entegrasyon/AABB/dar faz toplu işlerinde ~3×, çözücüde 1.5–2.5× makul **hipotez**; AGENTS gereği yalnız ölçümle iddia edilir.
10. **Bütçe aritmetiği:** K kare geri sarma için `fizik_adım ≤ 16.6 / (K+1)`; K=8 ⇒ ≈ 1.8 ms (120 Hz fizik, kare başına 2 adım ise adım başına ≈ 0.9 ms). Bu bütçe H8'in ölçütüdür.

### 4.3 Fizik için kıyas

- **Jolt:** ada tabanlı, katmanlı geniş faz, uyku, kısıtlar, **çapraz-platform belirlenimcilik seçeneği** (derleme bayrağı; `⚠ doğrulanmalı`: ad ve sınırlar); belirlenimciliği sıralı ada/temas işlemesi ve FMA'yı kapatmayla sağlar. MATOKI'nin avantajı: wasm'da FMA/ISA farkı zaten yok (relaxed hariç) — Jolt'un "bayrak" yükü bizde "yasak" olur.
- **Unreal Chaos:** varsayılan belirlenimci değil (`⚠ doğrulanmalı`); ağ fiziği için ayrı öngörü katmanı. **Bevy** fiziği harici (ör. Rapier; `⚠ doğrulanmalı`) — yani "birinci sınıf" fizik hedefi bizi Jolt çizgisine koyar.
- **Sonuç:** Belirlenimcilik + rollback birleşimi, fiziği **Jolt'tan bile katı** yapar; bu bizim ayırt edici özelliğimiz olabilir ama maliyet: tüm sim ofset tabanlı, sıralı ve testli olmak zorunda.

---

## 5. ARCH_V2'de eksik katman/kararlar

| Eksik | Neden önemli | Öneri |
|---|---|---|
| **Fizik tasarımı** (§4) | "Birinci sınıf" ama belgede yok | §4'ü ARCH_V2'ye L4 alt başlığı yap |
| **Extract/render proxy** ([D-02]) | Sim ∥ render yarışı | Açık düğüm |
| **Sim tick/zaman modeli** ([A-04]) | Rollback temeli | L0'a "tick zamanlayıcı" |
| **Girdi olay halkası** ([E-04]) | Rollback + worker motoru | SPSC kare-damgalı |
| **Servis katmanı (async)** ([D-01]) | IO/ağ/ses/derleme kare grafiği değil | L3'ü ikiye böl |
| **Varlık hattı (cook/paketleme/GUID/hot reload)** | "Sanal varlıklar" (`:70`) bir kaynak formatı ister; D-063 dış varlığı repoya almıyor (`DECISIONS.md:217`) | Cook aracı + paket formatı + sürümleme kararı |
| **GPU doku sıkıştırma/transcode** ([F-04]) | Aygıta bağlı biçimler | KTX2/Basis tarzı tasarım kararı |
| **Betik/oyun mantığı dili** | Oyun yazarı clang istemeden nasıl kod yazar? Ö-11 "genel sistem dili" beklemede (`DECISIONS.md:466-468`) | Karar: C+wasm AOT mi, Matoki genişletme mi |
| **Ağ stratejisi** | Yalnız "rollback"; fizik ağırlıklı/çok oyunculu ölçekte sunucu-yetkili anlık görüntü enterpolasyonu gerekebilir; WebRTC/WebTransport'un worker'da kullanılabilirliği `⚠ doğrulanmalı` | İki mod: rollback (küçük eş sayısı) + sunucu-yetkili |
| **Profil/yakalama araçları** | `core/profiler.h` 64 bölge, CPU, iş parçacıkları içinde kapalı (`profiler.h:1-3`; D-105 "Workers içinde PROF yok") | İkili komut akışı = ücretsiz yakalama/oynatma → araç yap |
| **Aygıt kaybı / sekme yaşam döngüsü** ([F-03]) | rAF kısıtlaması, arka plan sekmesi sim'i durdurur ⇒ rollback senkron kaybı | Tick zamanlayıcıda "duraklat/yeniden eşitle" protokolü |
| **Kalıcı depolama** | Kayıt/akış için OPFS/IndexedDB (OPFS senkron erişimi worker'da: `⚠ doğrulanmalı`) | Servis katmanında |
| **Test/CI matrisi** ([H-02]) | Tarayıcı + ST/MT + relaxed/non-relaxed | Headless CI |
| **Bellek bütçesi / cihaz sınıfı** ([E-01]) | Mobil gerçekçilik | Tablo |

## 6. Kıyas: Unreal / Frostbite / id Tech / Jolt / Bevy

(Genel bilinen mimari; sürüm ayrıntıları `⚠ doğrulanmalı`.)

| Konu | ARCH_V2 | Unreal | Frostbite | id Tech | Jolt | Bevy | Not |
|---|---|---|---|---|---|---|---|
| İş parçacığı | iş çalan deque + sınıf (planlı); bugün tek FIFO | Game/Render/RHI thread + TaskGraph | Job tabanlı | Ön uç/arka uç + iş parçacıkları (kökler 4/5) | Takılabilir JobSystem | Otomatik sistem paralelliği | Bizde yalnız render paralel |
| Sim/render ayrımı | Eksik (Extract yok) | Sahne proxy'leri | Çerçeve verisi çıkarımı | Ön uç komut üretimi | — | `Extract` + render world | [D-02] |
| Kare grafiği | CPU+GPU karma | RDG (bariyer + takma adlama) | FrameGraph (takma adlama + async compute) | Elle ayarlı geçişler | — | Render graph | WebGPU'da takma adlama yok ([G-02]) |
| Veri modeli | Arketip SoA (planlı) | UObject/Actor + Mass (ECS) | Veri güdümlü | Entity/idClass | — | Tablo + sparse hibrit | Hibrit öner ([F-01]) |
| Sanal varlık | Meshlet/sanal doku (Faz 3) | Nanite/VT | GPU-güdümlü | Sanal doku (megatexture mirası) | — | — | 64-bit atomik/sparse yok ([F-04]) |
| Fizik belirlenimciliği | "sim belirlenimci" (tek cümle) | Chaos: varsayılan değil | Sürüme bağlı | Sürüme bağlı | **Seçenekle çapraz-platform** | Harici | Bizim hedef Jolt'tan katı |
| Hesap dili | Matoki IR (CPU+GPU+ECS) | HLSL + C++ | HLSL + C++ | C++ + shader | C++ | Rust + WGSL | Tek dil iddiası en pahalı yer ([B-01]) |
| Çalışma anı derleme | Motor kendini derler | Shader permütasyon önbelleği | Shader permütasyonu | Shader | — | Pipeline önbelleği | Bizimki **kod** özelleştirme; yaşam döngüsü yok ([C-01]) |
| Ağ | Rollback (planlı) | Replikasyon + öngörü | Sunucu-yetkili | Sunucu-yetkili | — | Harici | Rollback = tam belirlenimcilik |

Çıkarımlar: (1) Hepsinin gerçek motorlarda ortak yanı **sim/render ayrımı için açık bir veri kopyalama katmanı**; (2) shader/kod özelleştirme "permütasyon önbelleği + asenkron derleme + disk önbelleği" ile yapılır, hiçbiri kare ortasında eşzamanlı derlemez; (3) belirlenimci fizik **özel bir mühendislik hedefidir** (Jolt'ta seçenek ve katı kural kümesi).

---

## 7. En yüksek riskli 5 karar

| # | Karar | Neden en riskli | Başarısızlık maliyeti | Azaltma |
|---|---|---|---|---|
| **1** | **Belirlenimcilik değişmezi + D-109 (relaxed SIMD, çoklu derleme, GPU seçimi) + işaretçisiz durum bölgesi** (ilke 5, §6) ([A-01], [A-02], [A-04], [G-01]) | Bugün hiçbir parça sağlamıyor; birbirleriyle çelişiyor; rollback ve ağ buna dayanıyor | Rollback netcode çalışmaz; geriye dönük tüm sistemlerin yerleşimi yeniden yazılır | Sınıflandırma (sim-kritik/görsel), D-036'yı güncelle, durum bölgesi prototipi + H7 (**ilk iş**) |
| **2** | **"Tek hesap dili": Matoki IR ile pixel + compute + ECS sistemi, CPU+GPU** (ilke 2, §3) ([B-01], [B-02]) | IR yeniden tasarımı, WGSL tekdüzelik, GPU sayısal farklılığı; derleyici zinciri (≈5k satır) hepsi etkilenir | Aylar süren yeniden yazım, D-032 kâhininin zayıflaması | Kısıtlı veri-paralel şablonlar; sınıflı H2; IR v2'yi ölçümle aç |
| **3** | **Çalışma anı özelleştirme: "motor kendini sahneye göre derler"** (ilke 3, §3) ([C-01], [C-03]) | JIT yaşam döngüsü yok; WGSL tarafında derleme çok pahalı/asenkron; gerçek kazanç belirsiz (D-042 öncülleri) | Sızıntı/donma/tablo tükenmesi; kazanç AOT ile aynı çıkarsa katman gereksiz | H1'i 3 kola böl; kademeli asenkron takas; anahtarda yalnız yapısal girdiler |
| **4** | **Zamanlayıcının CPU↔GPU'da ölçülen maliyetle iş seçmesi + 3 karelik boru hattı** (ilke 4, §5, §2) ([G-01], [E-07], [E-03]) | Geri okuma asenkron, ölçüm kaba, varyant matrisi 8 yol, gecikme ~50 ms | Belirlenimcilik kaybı, test patlaması, rollback hedefiyle çelişen gecikme | Statik atama + `sim_kritik` etiketi; derinlik yapılandırılabilir; Extract katmanı |
| **5** | **"Veri GPU'da yaşar" + sanal varlıklar + Faz 3 render v2 (Nanite/VSM/GI sınıfı)** (ilke 6, §4, §8) ([F-03], [F-04]) | WebGPU'da 64-bit atomik/sparse/bindless/takma adlama yok (`⚠`); aygıt kaybı; devasa kapsam | Aylarca ilerleme olmadan kapsam şişmesi | Küçük ölçülmüş dilimler; yetkili CPU kopyası; F0_E sonuçlarına bağla |

*Ek riskler (ilk 5'in dışında ama ciddi):* COOP/COEP barındırma kısıtı ve ST yedeğinin yokluğu ([E-05]); bellek tavanı 4 GB varsayımı ([E-01]); fizik kapsamının belgede olmaması (§4).

## 8. Kısa vadeli ürün riski notu

"Webin Unreal'i" vizyonu ile D-109'un "tavan = en yeni Chrome özellikleri" yaklaşımı çatışabilir: Chrome-dışı tarayıcılarda (Safari/Firefox, mobil) hangi özelliklerin hazır olduğu bilinmeden (F0_E) mimari kararları kilitlenmemeli (`⚠ doğrulanmalı`: Safari WebGPU/relaxed SIMD/OffscreenCanvas-WebGPU worker durumu).

---

## 9. İlk yapılması gerekenler (sıralı)

Bu inceleme koda dokunmaz; aşağıdakiler **yeni görev dosyası** olarak açılmalıdır.

1. **Belge hijyeni + D-110 (1–2 gün, kod yok):** belirlenimcilik **sınıfları** (sim-kritik / yalnız-görsel); D-036 durumunu "kapsamı daraltıldı" yap; ARCH_V2 §6'ya sınıf tablosu; `ARCHITECTURE.md:3,11` ve D-002/D-003 sayılarını düzelt; 4 GB ↔ 1 GB kararını yaz ([A-01], [J-01], [E-01]).
2. **H7 belirlenimcilik spike'ı (yeni görev, örn. `F0_G`):** mevcut fizik + ECS ile 10 000 tick hash; {1T, 2T, 8T}; Node + üç tarayıcı. Başarı ya da başarısızlık Faz 1'in şeklini belirler ([A-03], [A-05]).
3. **Durum bölgesi prototipi:** ofset tabanlı `RegionArena`, `StateRef`, ECS + fizik gövde dizisinin işaretçisiz küçük dilimi, anlık görüntü/geri yükleme/hash testi, `make check`'te durum başlığı `*` denetimi. F0_D'yi H4'ün yeni ölçütüyle ([E-02]) güncelle ([A-02], [I-01]).
4. **F0_E'yi öne al** (platform taraması): relaxed SIMD, Safari WebGPU, `Atomics.waitAsync` worker'da, OffscreenCanvas+WebGPU worker, 64-bit atomik, u64/sparse, `COEP: credentialless`, sync-compile sınırı. Sonuçlar D-109'u revize edebilir (§10 listesi).
5. **F0_B görevini güncelle** (başlamadan): H1 üç kol, derleme + çoğaltma gecikmesi p95, JIT yaşam döngüsü/sızıntı ölçütü (H9) ([C-01], [C-03]); JIT yaşam döngüsü tasarım notu (slot serbest listesi, asenkron takas).
6. **F0_C görevini güncelle:** Extract düğümü, iki kollu H3, servis katmanı ayrımı, iş zamanlayıcı v2 tasarım notu (deque + öncelik + `Atomics.waitAsync`) ([D-01], [D-02], [E-03], [G-03]).
7. **IR v2 kapsam kararı** (F0_A başlamadan): kısıtlı şablon mu IR v2 mi; M15_A için WGSL tekdüzelik kuralları; H2 sınıflı tolerans ([B-01], [B-02]).
8. **Küçük platform düzeltmeleri (ayrı küçük görevler):** worker sayısı formülü ([E-06]); ST derleme yedeği + yükleyici seçimi ([E-05]); girdi olay halkası ([E-04]); `platform.js:150-152` sessiz hata yutma.
9. **Fizik v2 görev paketi:** tasarım notu (§4.2) → SoA gövde durumu → sabit tick → kanonik geniş faz → ada + boyama çözücü → 1T=2T=8T CI; M16_A'nın kapsamı "gerçek rigid-body" ölçeğine göre yeniden yazılsın (bkz. §4).
10. **Headless tarayıcı CI kararı** (`DECISIONS`): test araçları için JS/npm istisnası ve altın görüntü/hash yöntemi ([H-02]).
11. **Faz 3'ü daralt** ([F-04]); render v2 yalnız GPU-sürümlü kırpma + dolaylı çizim + kümelenmiş ileri ile başlasın.

## 10. `⚠ doğrulanmalı` listesi (web platformu — F0_E'ye girdi)

1. Relaxed SIMD `madd`/`min/max` sonuçlarının uygulamaya bağlılığı (spec) ve LLVM'in `-mrelaxed-simd` ile skaler/vektör FMA daraltması.
2. wasm f32 NaN yük bitlerinin belirsizliği (nondeterminism notları).
3. WGSL: f32 kesinlik/FMA daralması/denormal; türev/örnekleme için kontrol akışı tekdüzeliği; `discard` anlambilimi.
4. WebGPU: 64-bit atomik yok mu, subgroups/timestamp-query/f16 durumu ve kaba çözünürlüğü, bindless/seyrek doku/çoklu dolaylı çizim, kaynak belleği takma adlama, `device.lost` senaryoları, `mapAsync` gecikmesi, varsayılan sınır değerleri (depolama tamponu sayısı/boyutu).
5. GPU doku sıkıştırma özellikleri (BC/ASTC/ETC2) aygıt dağılımı.
6. Chrome'da ana iş parçacığında eşzamanlı `WebAssembly.Module` boyut sınırı ve güncel durumu.
7. Derlenmiş `WebAssembly.Module`'ün `postMessage` ile worker'lara paylaşılırken tekrar derlenip derlenmediği.
8. Worker'da `Atomics.waitAsync` desteği; iç içe `setTimeout` kıstırması worker'larda.
9. `requestAnimationFrame` ve WebGPU + OffscreenCanvas'ın worker'da Safari/Firefox durumu; WebRTC'nin worker'da kullanılabilirliği; OPFS senkron erişim tutamaçları.
10. 4 GB paylaşımlı wasm belleği (mobil/Safari), `memory.grow` + paylaşımlı bellek görünüm yenileme maliyeti, `memory64` performans cezası.
11. COOP/COEP altında üçüncü taraf gömme/portal kısıtları ve `COEP: credentialless` desteği.
12. Tarayıcı birleştirici (compositor) gecikmesi, boru hattı derinliğinin girdi gecikmesine etkisi.
13. Kıyas bölümündeki motor ayrıntıları (Chaos belirlenimciliği, id Tech sürüm farkları, Jolt belirlenimcilik bayrağı adı, Bevy'nin wasm iş parçacığı durumu).

## 11. Okunan dosyalar (kanıt envanteri)

`AGENTS.md`; `docs/ARCH_V2.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md`, `docs/MATOKI.md`, `docs/ROADMAP.md`;
`tasks/F0_A..F0_F`, `tasks/M10_E`, `tasks/M13_B`, `tasks/M15_A`, `tasks/M15_B`, `tasks/M16_A`, `tasks/M16_B`;
`src/thread/{job.h,job.c,thread.h,thread.c,atomic.h}`, `src/core/{arena.h,arena.c,math.h,math.c}`, `src/scene/{ecs.h,ecs.c,scene_io.h}`,
`src/phys/{world.h,world.c,collide.h,bvh.h}`, `src/matoki/{ir.h,matoki.h,variant.h,variant.c,codegen.h}`, `src/jit/jit.c`,
`src/app/{main.c,shaders.c,scene_ocak.c}`, `src/gfx3d/pipeline.c`, `web/{platform.js,threads.js,worker.js,jit.js}`, `tools/serve.py`, `Makefile`, `tests/test_phys.c` (test listesi).
