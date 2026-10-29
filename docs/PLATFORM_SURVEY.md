# MATOKI Platform Gerçeklik Taraması (Ekim 2026)

Durum: KABUL EDİLDİ · 2026-10-29 · Görev: F0_E (H6 Kanıtlaması) · İlgili Kararlar: D-108, D-109, ARCH_V2

Bu belge, MATOKI Mimari v2 (ARCH_V2) ve D-109 ("Platform sınırlarını zorla: en yeni web özellikleri, tespit + yedek")
kapsamında hedeflenen web platformu yeteneklerinin **Ekim 2026** itibarıyla kararlı tarayıcı motorlarındaki
(Blink/Chromium, Gecko/Firefox, WebKit/Safari — masaüstü ve mobil) gerçek durumunu, eksikliklerini,
Unreal Engine 5 (Nanite, Lumen) sınıfı bir render mimarisinin WebGPU'daki darboğazlarını ve geçici çözümlerini belgeler.

---

## 1. Platform Özellikleri ve Tarayıcı Destek Matrisi

Tablodaki durum tanımları:
- **Destekli (Sürüm)**: Kararlı sürümde bayraksız, varsayılan olarak açık.
- **Kısmi / Donanım Bağımlı**: Yalnızca belirli işletim sistemi veya GPU sürücülerinde ya da özel donanımlarda açık.
- **Deneysel**: Flag arkasında, Origin Trial veya geliştirici sürümünde.
- **Yok**: Kararlı sürümde bulunmuyor; geliştirme veya standartlaşma aşamasında.

| # | Özellik / API | Chrome / Edge (Blink) | Firefox (Gecko) | Safari (WebKit) | Kararlılık / Baseline | Doğrulanmış Kaynak Bağlantısı |
|---|---|---|---|---|---|---|
| **1** | **WebGPU Core (Compute & Render)** | Destekli (v113+ masaüstü, v121+ Android 12+) | Kısmi / Destekli (v125+ Win, macOS Apple Silicon; Linux/Android arkada) | Destekli (v18+ macOS Sequoia, iOS 18+) | Geniş Tabanlı (Modern OS) | [MDN: WebGPU API](https://developer.mozilla.org/en-US/docs/Web/API/WebGPU_API) · [CanIUse: webgpu](https://caniuse.com/webgpu) · [ChromeStatus](https://chromestatus.com/feature/6213121689518080) |
| **2** | **WebGPU: timestamp-query** | Destekli (v121+; varsayılan 100 µs kuantize; dev flag ile hassas) | Kısmi / Flags (Zamanlama saldırısı kısıtları) | Sınırlı / Flags (Metal 3 profili, gizlilik kısıtı) | Tavan (Geliştirici Profiling) | [MDN: writeTimestamp](https://developer.mozilla.org/en-US/docs/Web/API/GPUCommandEncoder/writeTimestamp) · [W3C Spec §25.1](https://www.w3.org/TR/webgpu/#timestamp-query) · [ChromeStatus](https://chromestatus.com/feature/5195029587427328) |
| **3** | **WebGPU: subgroups** | Destekli (v144+ kararlı; v128+ deneysel) | Yok (Geliştirme sürüyor) | Yok (Desteklenmiyor) | Tavan (Chromium Özel) | [CanIUse: subgroups](https://caniuse.com/mdn-api_gpusubgroup_subgroups) · [W3C Subgroups Proposal](https://github.com/gpuweb/gpuweb/blob/main/proposals/subgroups.md) |
| **4** | **WebGPU: shader-f16** | Destekli (v120+ masaüstü; Android'de Vulkan VK_KHR_shader_float16_int8 varsa) | Kısmi (Masaüstü geliştirme, Gecko #1853443) | Destekli (v18.2+ / v26+ Apple Silicon Metal) | Tavan (Yüksek Uyumluluk) | [MDN: shader-f16](https://developer.mozilla.org/en-US/docs/Web/API/GPUAdapter/features) · [ChromeStatus](https://chromestatus.com/feature/5713437199269888) · [CanIUse: shader-f16](https://caniuse.com/mdn-api_gpuadapter_features_shader-f16) |
| **5** | **WebGPU: drawIndirect & indexed** | Destekli (v113+) | Destekli (v125+) | Destekli (v18+) | Baseline (WebGPU Core) | [MDN: drawIndirect](https://developer.mozilla.org/en-US/docs/Web/API/GPURenderPassEncoder/drawIndirect) · [W3C WebGPU Spec](https://www.w3.org/TR/webgpu/) |
| **6** | **WebGPU: indirect-first-instance** | Destekli (v113+) | Destekli (v125+) | Destekli (v18+) | Geniş Destek (Opsiyonel Feature) | [MDN: indirect-first-instance](https://developer.mozilla.org/en-US/docs/Web/API/GPUAdapter/features) · [W3C Spec §25.1](https://www.w3.org/TR/webgpu/#feature-indirect-first-instance) |
| **7** | **WebGPU: multiDrawIndirect / drawCount** | Yok / Deneysel (Dawn flag arkasında v131+) | Yok (W3C #1354 öneri aşamasında) | Yok (Desteklenmiyor) | Yok (Gelecek Sürüm) | [GPUWeb Issue #1354](https://github.com/gpuweb/gpuweb/issues/1354) · [GPUWeb Issue #4349](https://github.com/gpuweb/gpuweb/issues/4349) |
| **8** | **WebGPU: Bindless / Unbounded Arrays** | Yok (Milestone 2 hedefi, aktif prototip) | Yok (Milestone 2) | Yok (Milestone 2) | Yok (Gelecek Sürüm) | [GPUWeb Issue #380](https://github.com/gpuweb/gpuweb/issues/380) · [GPUWeb Issue #5517](https://github.com/gpuweb/gpuweb/issues/5517) · [Bindless Proposal](https://github.com/gpuweb/gpuweb/blob/main/proposals/bindless.md) |
| **9** | **WebGPU: 64-bit atomik (atomic<u64>)** | Yok (Standart skaler yok; `atomic-vec2u-min-max` Chromium prototip) | Yok (Standart yok) | Yok (Metal donanım kısıtlaması) | Yok (Prototip / Öneri) | [GPUWeb Issue #5071](https://github.com/gpuweb/gpuweb/issues/5071) · [atomic-64-min-max Proposal](https://github.com/gpuweb/gpuweb/blob/main/proposals/atomic-64-min-max.md) · [ChromeStatus](https://chromestatus.com/feature/5149301297545216) |
| **10** | **Wasm: relaxed SIMD** | Destekli (v114+ masaüstü & Android) | Destekli (v146+ masaüstü & Android) | Yok (WebKit Bug #258908, geliştirme) | Tavan (Chromium + Gecko) | [CanIUse: wasm-relaxed-simd](https://caniuse.com/wasm-relaxed-simd) · [WebAssembly Relaxed SIMD Proposal](https://github.com/WebAssembly/relaxed-simd) |
| **11** | **Wasm: Threads (SharedArrayBuffer + Atomics)** | Destekli (v70+, COOP/COEP ile v91+) | Destekli (v79+ COOP/COEP) | Destekli (v14.1+ / iOS 14.5+ COOP/COEP) | Baseline (Tam Çapraz Destek) | [MDN: SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer) · [CanIUse: sharedarraybuffer](https://caniuse.com/sharedarraybuffer) |
| **12** | **Wasm: memory64** | Destekli (v133+ masaüstü & Android) | Destekli (v134+ masaüstü & Android) | Yok (WebKit Bug #267158, beklemede) | Tavan (Chromium + Gecko) | [CanIUse: wasm-memory64](https://caniuse.com/wasm-memory64) · [ChromeStatus](https://chromestatus.com/feature/5134704870490112) · [WebAssembly Memory64](https://github.com/WebAssembly/memory64) |
| **13** | **Wasm: JSPI (JavaScript Promise Integration)** | Destekli (v137+; Phase 4) | Destekli (v153+ kararlı) | Destekli (v27+ WebKit motoru) | Yakın Gelecek Baseline | [CanIUse: wasm-jspi](https://caniuse.com/wasm-jspi) · [WebAssembly JSPI Repo](https://github.com/WebAssembly/js-promise-integration) |
| **14** | **Wasm: multi-memory** | Destekli (v120+) | Destekli (v125+) | Kısmi / In Progress (WebKit Bug #277743) | Tavan (Chromium + Gecko) | [ChromeStatus](https://chromestatus.com/feature/5107937409597440) · [WebAssembly Multi-Memory Repo](https://github.com/WebAssembly/multi-memory) |
| **15** | **Wasm: tail calls** | Destekli (v112+) | Destekli (v121+) | Destekli (v18.2+) | Baseline (Evrensel Destek) | [CanIUse: wasm-tail-calls](https://caniuse.com/wasm-tail-calls) · [WebAssembly Tail Calls Repo](https://github.com/WebAssembly/tail-call) |
| **16** | **Wasm: GC (Garbage Collection)** | Destekli (v119+) | Destekli (v120+) | Destekli (v18.2+) | Baseline (Evrensel Destek) | [CanIUse: wasm-gc](https://caniuse.com/wasm-gc) · [WebAssembly WasmGC Repo](https://github.com/WebAssembly/gc) |
| **17** | **Web: Atomics.waitAsync** | Destekli (v90+) | Destekli (v145+) | Destekli (v16.4+ macOS & iOS) | Baseline (Evrensel Destek) | [MDN: Atomics.waitAsync](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Atomics/waitAsync) · [CanIUse: waitAsync](https://caniuse.com/mdn-javascript_builtins_atomics_waitasync) |
| **18** | **Web: OffscreenCanvas + WebGPU Worker'da** | Destekli (v113+) | Kısmi / Destekli (v125+ masaüstü; Android sınırlı) | Destekli (v18+ macOS Sequoia / iOS 18+) | Geniş Tabanlı (Modern OS) | [MDN: OffscreenCanvas](https://developer.mozilla.org/en-US/docs/Web/API/OffscreenCanvas) · [W3C WebGPU Worker Spec](https://www.w3.org/TR/webgpu/#dom-canvas-transfercontroltooffscreen) |
| **19** | **Web: WebCodecs (AV1 Çözme)** | Destekli (v94+; donanım + libdav1d yazılım) | Destekli (v130+ masaüstü dav1d; Android yok/kısıtlı) | Kısmi / Donanım Bağımlı (v16.4+; **yalnız** M3+, A17 Pro/iPhone 15 Pro+, iPhone 16+; yazılım fallback YOK) | Parçalı (Apple Donanım Şart) | [MDN: VideoDecoder](https://developer.mozilla.org/en-US/docs/Web/API/VideoDecoder) · [CanIUse: webcodecs](https://caniuse.com/webcodecs) |
| **20** | **Web: WebTransport (HTTP/3 QUIC)** | Destekli (v97+) | Destekli (v114+) | Destekli (v16.4+) | Baseline (Evrensel Destek) | [MDN: WebTransport API](https://developer.mozilla.org/en-US/docs/Web/API/WebTransport_API) · [CanIUse: webtransport](https://caniuse.com/webtransport) |
| **21** | **Web: WebNN (Web Neural Network API)** | Kısmi / Deneysel (Origin Trial / DirectML / CoreML arkasında) | Yok (Standartlaşma pozisyonu olumlu, kapalı) | Yok (Desteklenmiyor) | Deneysel / Yok | [CanIUse: webnn](https://caniuse.com/webnn) · [ChromeStatus](https://chromestatus.com/feature/5598642730106880) · [W3C WebNN Spec](https://www.w3.org/TR/webnn/) |

---

## 2. Ayrıntılı Özellik Analizleri

### 2.1 WebGPU Çekirdeği ve Gelişmiş Uzantılar

#### Compute Shader
- **Durum:** Tüm ana tarayıcılarda (Chrome 113+, Firefox 125+, Safari 18+) kararlı.
- **Kullanım:** Matoki IR'dan WGSL compute çekirdeği üretimi tam desteklenir. GPU tabanlı parçacık simülasyonu, culling, animasyon skinning ve hesaplama işleri ana platformlarda taşınabilirdir.

#### Timestamp Queries (`timestamp-query`)
- **Durum:** Chrome 121+ üzerinde `timestamp-query` aygıt özelliği olarak mevcuttur. Ancak Spectre/fingerprinting güvenlik önlemleri nedeniyle zaman damgaları **100 mikrosaniye (0.1 ms)** düzeyine kuantize edilir (yuvarlanır).
- **Geliştirici Hassasiyeti:** Tam nanosaniye hassasiyeti için Chromium bayrağı `--enable-webgpu-developer-features` veya `chrome://flags` gereklidir. Safari ve Firefox'ta gizlilik politikaları nedeniyle kararlı kanalda standart dışıdır.
- **Mimari Karar:** Motor profiling altyapısı kuantize değerleri tolere etmeli, mikro-benchmark'lar için CPU tarafı yüksek hassasiyetli `performance.now()` veya native test araçları referans alınmalıdır.

#### Subgroups (`subgroups`)
- **Durum:** Chromium (Chrome/Edge 144+) Dawn üzerinde tam desteklidir. WGSL içinde `enable subgroups;` direktifiyle `subgroupElect`, `subgroupAdd`, `subgroupBallot`, `subgroupShuffle` fonksiyonları kullanılabilir. Firefox ve Safari'de henüz bulunmamaktadır.
- **Mimari Karar:** Tavan olarak ele alınmalı, tespit edildiğinde workgroup içi paralel indirgemelerde (reduction/prefix-sum) kullanılmalı; yoksa workgroup paylaşımlı bellek (`var<workgroup>`) yedeğine dönülmelidir.

#### 16-Bit Kayan Nokta (`shader-f16`)
- **Durum:** Chrome 120+ masaüstünde, Safari 18.2+ Apple Silicon sistemlerde (Metal natively fp16 çalışır) etkindir. Android cihazlarda Vulkan donanım sürücüsünün `VK_KHR_shader_float16_int8` ve 16-bit storage uzantısını sunmasına bağlıdır.
- **Mimari Karar:** Bellek bant genişliği ve matematik verimi için kritik bir kazançtır. Destekleyen cihazlarda WGSL `enable f16;` varyantı sunulmalı; desteklemeyen cihazlar için standart f32 varyantı derlenmelidir.

#### Bindless ve Kaynak Dizileri
- **Durum:** WebGPU V1 şartnamesinde **bindless (sınırsız tanımlayıcı tabloları)** yer almaz. Bind group başına sıkı kaynak sınırları mevcuttur (ör. max 16 sampleable texture, max 16 sampler, max 8 storage buffer).
- **Standartlaşma:** W3C GPUWeb Milestone 2 kapsamında (Issue #380, #5517) `proposals/bindless.md` taslağı ilerlemektedir.
- **Mimari Çözüm:** §3'te detaylandırılan Sanal Doku Sayfaları (`Virtual Texturing`) ve `Texture2DArray` yapıları kullanılmalıdır.

#### Indirect Çizim ve GPU Sayacı
- **Durum:** `drawIndirect`, `drawIndexedIndirect`, `dispatchWorkgroupsIndirect` ve `indirect-first-instance` V1 standardında mevcuttur.
- **Eksiklik:** GPU tamponunda tutulan dinamik sayaçla çizim yapan `drawIndirectCount` veya `multiDrawIndirect` (W3C #1354, #4349) kararlı sürümlerde yoktur.
- **Mimari Çözüm:** Tavanlı indirect çizim (bounded indirect buffers) ve GPU culling sonrası `instanceCount = 0` atanarak GPU tarafından anında elenen çağrılar.

#### 64-Bit Atomik Operasyonlar
- **Durum:** WGSL'de genel `atomic<u64>` veya `atomic<i64>` tipleri **yoktur**.
- **Neden Yok?** Apple Metal Shading Language (MSL) donanım mimarilerinde genel 64-bit atomikler (add, exchange) kısıtlıdır; ancak 64-bit `atomic_min` ve `atomic_max` desteklenmektedir.
- **Çözüm Önerisi:** W3C GPUWeb grubu `atomic-vec2u-min-max` (`gpuweb/proposals/atomic-64-min-max.md`, Issue #5071) önerisiyle `atomic<vec2u>` tipi üzerinden 64-bit min ve max operasyonlarını sunmayı hedeflemektedir. Chromium Canary'de deneyseldir.

---

### 2.2 WebAssembly Gelişmiş Özellikleri

#### Relaxed SIMD (`wasm-relaxed-simd`)
- **Durum:** Chrome/Edge 114+ ve Firefox 146+ masaüstü ile mobilde tam desteklidir. Safari'de (WebKit Bug #258908) henüz desteklenmemektedir.
- **MATOKI Politikası (D-036 ve D-109):** D-036'da donanıma göre deterministik olmayan sonuçlar nedeniyle Matoki JIT için reddedilmişti. Ancak D-109 gereği tavan modunda oyun fiziği dışındaki render/grafik hesaplamaları veya platform tespitine bağlı özel çekirdekler için değerlendirilebilir. Motor `engine_relaxed.wasm` varyantını JS yetenek tespitine göre dinamik yükleyebilir.

#### Threads (`SharedArrayBuffer` + Wasm Atomics)
- **Durum:** Tüm tarayıcılarda (Chrome, Firefox, Safari macOS/iOS) kararlıdır. Tek zorunluluk HTTP başlıklarında `Cross-Origin-Opener-Policy: same-origin` ve `Cross-Origin-Embedder-Policy: require-corp` bulunmasıdır (`tools/serve.py` zaten bunu uygular).
- **Kullanım:** MATOKI Faz 2 işçi (worker) havuzu, kilitsiz iş kuyrukları ve paylaşımlı bellek mimarisi için evrensel tabandır.

#### Memory64 (`wasm-memory64`)
- **Durum:** Chrome 133+ ve Firefox 134+ tarafından desteklenmektedir. Safari'de (WebKit Bug #267158) henüz yoktur.
- **Kullanım:** 4 GB adresleme sınırını aşarak büyük açık dünya varlıklarının ve sanal doku sayfalarının tek doğrusal bellekte tutulmasına olanak tanır. Ancak Safari desteği olmadığı için varsayılan ikili `wasm32` kalmalı, `wasm64` tavan varyantı olarak derlenmelidir.

#### JSPI / Stack Switching
- **Durum:** Chrome 137+, Safari 27+ ve Firefox 153+ üzerinde kararlı aşamadadır.
- **Kullanım:** Senkron çalışan C/Wasm kodunun tarayıcı Promise/fetch/IndexedDB çağrılarında yürütmeyi askıya alıp (suspend) devam ettirmesini (resume) sağlar (Asyncify yükü olmadan). Varlık yükleyicileri için gelecekte büyük kolaylıktır.

#### Tail Calls (`return_call`, `return_call_indirect`)
- **Durum:** Chrome 112+, Firefox 121+, Safari 18.2+ ile **Baseline** düzeyindedir. Tüm modern tarayıcılarda güvenle kullanılabilir.
- **Kullanım:** Yığın tüketmeden sonsuz durum makineleri, yorumlayıcılar ve Matoki derleyicisinin kuyruk optimizasyonları için hazırdır.

#### Wasm GC
- **Durum:** Chrome 119+, Firefox 120+, Safari 18.2+ ile **Baseline** düzeyindedir.
- **Not:** MATOKI C11 freestanding ve arena tabanlı bellek kullandığından çekirdek motorda Wasm GC kullanılmaz. Ancak ileride web tabanlı editör araçları veya betik dili entegrasyonu için referans olarak izlenmelidir.

---

### 2.3 Web Platform API'leri ve Çalışma Zamanı

#### `Atomics.waitAsync`
- **Durum:** Chrome 90+, Firefox 145+, Safari 16.4+ ile tam çapraz tarayıcı uyumludur.
- **Kullanım:** Ana iş parçacığı (main thread) bloke edilemediği için (`Atomics.wait` ana thread'de hata fırlatır), `Atomics.waitAsync` ana thread ile işçi thread'ler arasında kilitsiz, asenkron bildirim almak için idealdir.

#### OffscreenCanvas + WebGPU Worker'da
- **Durum:** Chrome 113+ ve Safari 18+'de eksiksiz; Firefox'ta masaüstünde kararlı.
- **Kullanım:** ARCH_V2 §2'de tanımlanan "Ana iş parçacığı yalnızca DOM/girdi köprüsüdür; render worker OffscreenCanvas ve WebGPU cihazının sahibidir" mimarisi tam olarak uygulanabilir durumdadır.

#### WebCodecs (AV1 Çözme)
- **Durum:** Chrome ve Firefox masaüstünde yazılımsal (`libdav1d`) çözücü sayesinde her donanımda AV1 çözer.
- **Safari Sınırı:** Safari 16.4+ WebCodecs destekler ancak AV1 için **yazılımsal fallback sunmaz**. Yalnızca donanımında AV1 kod çözücü çip bulunan cihazlarda (Apple M3, M4, A17 Pro / iPhone 15 Pro, iPhone 16) çalışır. M1/M2 Mac veya standart iPhone'larda hata döner.
- **Mimari Çözüm:** Video dokular veya ara sahneler için AV1 birinci tercih, H.264 / HEVC ise Safari taban yedeği olmalıdır.

#### WebTransport (HTTP/3)
- **Durum:** Chrome 97+, Firefox 114+, Safari 16.4+ ile **Baseline** standardındadır.
- **Kullanım:** Çok oyunculu rollback netcode (güvenilmez datagram'lar) ve arka plan varlık akışı (güvenilir stream'ler) için TCP/WebSocket yerine doğrudan kullanılabilir.

#### WebNN (Web Neural Network API)
- **Durum:** Yalnızca Chromium tabanlı tarayıcılarda deneysel (Origin Trial / DirectML / CoreML) aşamasındadır. Firefox ve Safari'de henüz kullanıcıya açık sürüm yoktur.
- **Mimari Karar:** ARCH_V2 Faz 1/2 kapsamı dışında tutulmalı, beklemede kalmalıdır.

---

## 3. WebGPU'da Olmayan Unreal-Sınıfı (Nanite / Lumen) Render Özellikleri ve Geçici Çözümleri

Unreal Engine 5'in temel teknolojileri (Nanite mikropoligon geometrisi, Lumen dinamik global ışıklandırması ve Virtual Shadow Maps), masaüstü DirectX 12 Ultimate ve Vulkan donanım uzantılarına dayanır. Bu yeteneklerin WebGPU'daki karşılıkları ve MATOKI için geçici çözüm stratejileri:

```
+------------------------------------+--------------------------+-------------------------------------------------+
| Unreal 5 Yeteneği                  | WebGPU V1'deki Durumu   | MATOKI ARCH_V2 Geçici Çözümü (Workaround)       |
+------------------------------------+--------------------------+-------------------------------------------------+
| 1. Mesh Shaders & Task Shaders     | Desteklenmiyor           | Compute Rasterizer (Yazılımsal) + GPU Culling   |
| 2. 64-bit Atomics (InterlockedMin) | Standart Değil           | İki Geçişli Derinlik+ID veya atomic-vec2u       |
| 3. Bindless Resources              | Yok (Milestone 2)        | Texture2DArray + Virtual Texturing (Sayfalama)  |
| 4. drawIndirectCount               | Yok                      | Sabit Üst Sınırlı Indirect + No-Op Draws        |
| 5. Hardware Ray Tracing (DXR)      | Yok (Post-V1)            | Software Lumen: Mesh SDF + Screen-Space GI      |
| 6. Built-in Barycentrics           | Yok (WGSL Issue #5566)   | Piksel Pozisyonundan 2D Kenar Formülüyle Çözüm  |
| 7. Conservative Rasterization      | Yok                      | Köşe Genişletme (Screen Guard) / Compute Raster |
| 8. Subgroup Shuffle / Ballot       | Kısmi (Yalnız Chromium)  | Workgroup Shared Memory İndirgemesi             |
+------------------------------------+--------------------------+-------------------------------------------------+
```

### 3.1 Mesh Shaders ve Task/Amplification Shaders
- **Gereksinim:** Nanite, geometriyi 128 üçgenlik "meshlet" (küme) parçalarına böler. Task shader kümeleri görünürlük, hiyerarşik Z-tamponu (HZB) ve ekran boyutu LOD'una göre eler; hayatta kalanları mesh shader üçgenlere dönüştürür.
- **WebGPU Durumu:** Standartta mesh/task shader hattı yoktur (W3C Issue #3015).
- **Geçici Çözüm:**
  1. *Compute Culling + Indirect Index Draw:* Bir compute pass'i meshlet AABB sınırlarını HZB'ye göre eler. Görünür meshlet'lerin indeks aralıklarını bir indirect argüman buffer'ına yazar. Standart `drawIndexedIndirect` ile rasterize edilir.
  2. *Compute Software Rasterizer:* Nanite'ın 4 pikselden küçük üçgenler için kullandığı yaklaşımın aynısı: Üçgen kurulumu ve rasterizasyon doğrudan bir compute shader içinde 8×8 piksel karolara paralel yapılır. WebGPU compute workgroup'ları bu işlem için son derece uygundur.

### 3.2 64-Bit Atomik Operasyonlar (Nanite 64-bit Visibility Buffer)
- **Gereksinim:** Nanite Visibility Buffer mimarisinde her piksele 64-bit veri atomik olarak yazılır:
  `[Derinlik (32-bit float/uint) | Instance ID (25-bit) + Üçgen ID (7-bit)]`.
  GPU `InterlockedMin` fonksiyonuyla hem early-Z derinlik testini yapar hem de en önde kalan üçgen kimliğini tek işlemde kaydeder.
- **WebGPU Durumu:** WGSL'de `atomic<u64>` yoktur. `atomic-vec2u-min-max` önerisi henüz kararlı değildir.
- **Geçici Çözüm:**
  1. *İki Geçişli Derinlik ve Kimlik (Two-Pass Depth + ID):*
     - Geçiş 1: Standart WebGPU donanımsal derinlik tamponuna normal derinlik yazılır (`DEPTH_LESS`).
     - Geçiş 2: Derinlik testi `EQUAL` (veya toleranslı `LESS_EQUAL`) ayarlanarak, piksel shader'da 32-bit `u32` renk tamponuna yalnız görünür Instance/Triangle ID yazılır.
  2. *Kuantize Hibrit Paketleme:* 20-bit derinlik + 12-bit ID (yalnızca küçük sahnelerde veya sınırlı nesne sayısında geçerlidir).
  3. *Tavan Modu:* Cihazda Chromium prototipi `atomic-vec2u-min-max` tespit edilirse tek geçişli 64-bit atomik koda geçilir.

### 3.3 Bindless Doku ve Tanımlayıcı Tabloları (Unbounded Descriptor Indexing)
- **Gereksinim:** Visibility buffer çözümlenirken (material evaluate pass), ekrandaki pikseller binlerce farklı materyal ve dokuya rastgele erişir. Geleneksel bind group'lar yetersizdir.
- **WebGPU Durumu:** Sınırsız doku dizileri ve bindless indeksleme yoktur.
- **Geçici Çözüm:**
  1. *Texture2DArray:* Benzer çözünürlük ve formattaki dokular (ör. 1024×1024 BC7 veya RGBA8) dizi dokularda toplanır; shader 3. koordinat olarak dilim indeksini (`slice_index`) kullanır.
  2. *Sanal Dokulama / Sayfalama (Virtual Texturing):* Tüm sahne dokuları 128×128 texel'lik sayfalara bölünür. GPU'da sabit boyutlu büyük bir fiziksel atlas (ör. 4096×4096) ve bir sayfa tablosu (indirection texture) tutulur. Görünür sayfalar CPU/akış önbelleği tarafından talep üzerine yüklenir.
  3. *Materyal Kimliğine Göre Kümeleme:* Tamponlar materyal türüne göre sıralanır, bind group değişimleri minimize edilir.

### 3.4 GPU Güdümlü Çizim Sayacı (`drawIndirectCount`)
- **Gereksinim:** GPU culling sonrasında hayatta kalan çizim sayısını CPU bilmez. Çizim sayısı doğrudan GPU tamponundaki bir sayaçtan okunmalıdır.
- **WebGPU Durumu:** `drawIndirect` argümanları tampondan alır ancak toplam çizim çağrısı sayısı CPU tarafında JavaScript ile belirtilmek zorundadır.
- **Geçici Çözüm:**
  1. *Tavanlı Çizim ve Dejenere Çağrılar:* CPU, maksimum olası nesne sayısı kadar (ör. 10.000) indirect çizim çağrısını döngüyle kaydeder (`encoder.drawIndirect(buffer, offset)`). GPU culling compute shader'ı, elenen nesnelerin argümanlarındaki `instanceCount` veya `indexCount` değerini `0` yapar. Modern GPU'lar sıfır indeksli çağrıları anında atlar (overhead ihmal edilebilir düzeydedir).

### 3.5 Donanımsal Işın İzleme (Hardware Ray Tracing / Lumen)
- **Gereksinim:** Dinamik global aydınlatma, çoklu yansımalar ve gölgeler için BVH hızlandırma yapıları ve donanım RT çekirdekleri.
- **WebGPU Durumu:** Ray tracing pipeline'ları veya inline ray query WebGPU V1'de yoktur (araştırma grubunda tartışılmaktadır).
- **Geçici Çözüm (Software Lumen Eşdeğeri):**
  1. *Mesh Signed Distance Fields (SDF):* Her nesne için düşük çözünürlüklü 3D SDF dokuları oluşturulur. Bir compute shader, sahne SDF hacmi üzerinde sphere tracing (ray marching) yaparak yumuşak gölgeleri ve dolaylı ortam aydınlatmasını hesaplar.
  2. *Voksel Koni Işınlama (Voxel Cone Tracing) / ReSTIR:* Sahne kaba bir 3D voksel ızgarasına enjekte edilir; compute shader voksel ızgarasından ışık toplar.
  3. *Ekran Uzayı Global Aydınlatma (SSGI):* Kamera görüş alanındaki pikseller için derinlik ve normal tamponları üzerinden hiyerarşik ışın izleme yapılır.

### 3.6 Yerleşik Barycentric Koordinatları (`SV_Barycentrics`)
- **Gereksinim:** Visibility buffer çözümlerken pikselin üçgen içindeki UV, normal ve teğet vektörlerini enterpole etmek için piksel merkezinin ağırlıklı koordinatları gerekir.
- **WebGPU Durumu:** WGSL'de `@builtin(barycentrics)` yoktur (W3C Issue #5566).
- **Geçici Çözüm:**
  - Vertex tamponundan üçgenin 3 köşesinin klip/ekran uzayı koordinatları (`v0, v1, v2`) compute/material shader içinde doğrudan okunur. Pikselin ekran pozisyonu (`@builtin(position).xy`) ile 2D kenar fonksiyonları (edge equations) çözülerek analitik barycentric koordinatlar hesaplanır:
    $$\lambda_i = \frac{\text{alan}(P, V_j, V_k)}{\text{alan}(V_0, V_1, V_2)}$$
    Bu işlem birkaç FMA talimatı kadar ucuzdur ve Nanite'ın compute materyal çözümleme pass'inde halihazırda kullandığı yöntemdir.

### 3.7 Conservative Rasterization
- **Gereksinim:** Piksele en ufak temas eden üçgenlerin bile fragment üretmesini sağlayarak mikropoligonların ve ince tellerin yok olmasını önlemek (Virtual Shadow Maps için gereklidir).
- **WebGPU Durumu:** Donanımsal conservative rasterization desteği yoktur.
- **Geçici Çözüm:**
  - Vertex shader'da üçgenin ekran uzayı kenarları yarı piksel ($\pm 0.5$ piksel) dışarı doğru genişletilir (bounding guard band expansion).
  - Compute tabanlı yazılımsal rasterizer'da karo kapsama testi AABB-üçgen kesişim testiyle muhafazakar hale getirilir.

### 3.8 Subgroup Dalga İçi İletişim (Warp / Wavefront Reductions)
- **Gereksinim:** Prefix sum, kümeleme ve paralel indirgeme işlerinde thread'lerin bellek erişimi yapmadan doğrudan donanım yazmaçları üzerinden haberleşmesi.
- **WebGPU Durumu:** Chromium'da mevcuttur; Safari ve Firefox'ta yoktur.
- **Geçici Çözüm:**
  - Shader içinde workgroup paylaşımlı bellek (`var<workgroup> scratch: array<u32, 64>;`) ve `workgroupBarrier()` kullanılarak iki adımlı ağaç indirgemesi (tree reduction) yapılır.

---

## 4. ARCH_V2 İçin Mimari Öneriler ve Yol Haritası

D-109 ilkesi ("Tavanı hedefle, taban yalnız çalışsın") ve bu tarayıcı gerçekliği taramasına dayanarak MATOKI mimarisi için önerilen kurallar:

### 4.1 Tavan ve Taban Ayrımı

```
+--------------------------------------------------------------------------------------------------+
|                                    TAVAN (CHROME / FIREFOX)                                      |
|  - WebGPU (Compute, shader-f16, subgroups [Chromium], timestamp-query)                           |
|  - Wasm Relaxed SIMD (engine_relaxed.wasm)                                                       |
|  - Wasm memory64 (engine_relaxed64.wasm, 4 GB üzeri adresleme)                                   |
|  - OffscreenCanvas + WebGPU Worker (ayrık render thread'i)                                       |
|  - WebTransport (düşük gecikmeli HTTP/3 rollback ağı)                                            |
|  - WebCodecs AV1 donanım/yazılım çözme                                                           |
+--------------------------------------------------------------------------------------------------+
                                                |
                                      Dinamik Yetenek Tespiti
                                                v
+--------------------------------------------------------------------------------------------------+
|                                   TABAN / YEDEK (SAFARI / ESKİ)                                  |
|  - WebGPU Core / Yoksa CPU Software Rasterizer (D-108 RHI)                                       |
|  - Wasm SIMD128 + wasm32 (engine_simd32.wasm, 4 GB sınır)                                        |
|  - Two-Pass Visibility Buffer (Derinlik + EQUAL ID pass)                                         |
|  - Texture2DArray ve Sanal Doku Sayfaları (Virtual Texturing)                                    |
|  - WebTransport yoksa WebSocket / WebRTC DataChannel yedeği                                      |
|  - WebCodecs AV1 yoksa H.264 / HEVC video akışı                                                  |
+--------------------------------------------------------------------------------------------------+
```

### 4.2 L0 & L1 (Çalışma Zamanı ve Derleyici) Kararları
1. **İkili Modül Seçimi (JS Platform Katmanı):**
   - JS yükleyici (`web/platform.js`) açılışta `WebAssembly.validate` ve özellik tespiti ile tarayıcının yeteneklerini belirler:
     - `relaxed-simd` + `memory64` varsa $\rightarrow$ `build/engine_tavan.wasm`
     - Yalnızca `simd128` varsa $\rightarrow$ `build/engine_taban.wasm`
   - Motor çekirdeği C11 freestanding kodunda bu ayrım derleme bayraklarıyla (`-msimd128` vs `-mrelaxed-simd -memory64`) sağlanır.
2. **WebGPU RHI ve Komut Akışı (D-108):**
   - WebGPU cihazı doğrudan Render Worker içinde oluşturulmalıdır. Ana thread yalnızca girdi ve DOM olaylarını SharedArrayBuffer üzerinden worker'a aktarır.
   - WebGPU bulunamayan veya başlatılamayan ortamlarda RHI, CPU software rasterizer'a yönlenir; deterministik CPU render hattı korunur.
3. **Matoki IR $\rightarrow$ WGSL Çekirdekleri (L1):**
   - Compute pass çekirdekleri tüm tarayıcılarda ortak WGSL çıktısı üretir.
   - `shader-f16` desteği varsa `enable f16;` varyantı, yoksa f32 varyantı çalışma anında derlenir.
   - `subgroups` mevcutsa WGSL varyantında dalga içi intrinsic'ler kullanılır; yoksa workgroup paylaşımlı bellek yoluna gidilir.

### 4.3 L2 & L4 (Render ve Veri Mimarisi) Kararları
1. **Visibility Buffer Tasarımı:**
   - 64-bit atomiklerin evrensel olmaması nedeniyle, temel Visibility Buffer implementasyonu **Two-Pass Depth + ID** olarak tasarlanmalıdır.
   - Chromium ortamında `atomic-vec2u-min-max` algılanırsa tek geçişli 64-bit atomik min boru hattına geçilmelidir.
   - Barycentric enterpolasyonu shader içinde piksel koordinatlarından 2D analitik kenar denklemiyle hesaplanmalıdır (yerleşik donanım desteği beklenmeyecektir).
2. **GPU Sahnesi ve Materyaller:**
   - Bindless eksikliği nedeniyle tüm materyal dokuları **Texture2DArray** dilimlerine veya **Sanal Doku Sayfaları** (Virtual Texture Atlas) mimarisine oturtulmalıdır.
3. **Ağ Katmanı:**
   - Çok oyunculu rollback mimarisinde WebTransport varsayılan iletişim kanalı olarak seçilmelidir (Chrome, Firefox, Safari kararlı).
