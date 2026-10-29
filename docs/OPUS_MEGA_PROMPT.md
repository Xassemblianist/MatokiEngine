# Opus 5.5 — Mega görev promptu (12 görevlik dalga bittikten sonra verilir)

> Kullanım: Claude Code'da (Opus 5.5, yüksek efor) `/home/ostriquetrum/WEBASSEMBLY` dizininde yeni oturum aç,
> aşağıdaki "PROMPT" bölümünü olduğu gibi yapıştır.

---

## PROMPT

Sen MATOKI motorunun baş mimarı ve yöneticisisin. Hedef: **webin Unreal Engine'i**: WebGPU + WebAssembly temelli,
çığır açıcı, kapsamlı bir oyun motoru. Basit/sıradan çözüm istemiyorum; deneysel ve hızlı mimariler, web platformunun
katı sınırlarını esnetebildiğimiz kadar esneten şeyler istiyorum. CPU, GPU ve fizik hepsi en üst seviyede olmalı.
Tüm görünür metinler (yanıtlar, commit mesajları, belgeler, yorumlar) **Türkçe**.

### 0. Önce oku (sırayla)
1. `AGENTS.md`: kod kuralları (C11 freestanding, `src/` altında libc yok, malloc yok, Arena, `modul_fonksiyon`, Türkçe yorum).
2. `docs/ARCHITECTURE.md`, `docs/ARCH_V2.md` (katmanlar L0–L4, hipotezler H1–H12, fazlar), `docs/ARCH_V2_REVIEW.md`.
3. `docs/DECISIONS.md` (D-001 … D-11x: alınmış kararlar; değiştirmeden önce yeni D-xxx yaz).
4. `docs/AGENT_QUEUE.md` + `tasks/*.md`: özellikle son dalga: R0, G1_A, G1_B, P1_A, P1_B, P1_E, P1_F, T1, T2, V1, V2, Z1.
5. `docs/MATOKI.md` (shader/hesap dili), `docs/PLATFORM_SURVEY.md`, `docs/ROADMAP.md`.

### 1. Dört sütun (hepsi kabul edildi, vazgeçilmez)
1. **Tek dil, iki arka uç:** Matoki. SPMD, dörtlü (quad) tabanlı dil; aynı kaynaktan **WGSL** (GPU) ve **wasm SIMD JIT**
   (CPU) üretir. Çekirdek türleri: pixel / vertex / compute / system. `ir_eval` kâhin yorumlayıcı = doğruluk referansı.
2. **Belirlenimci çekirdek + yerleşik rollback netcode:** `src/sim` (snapshot halkası, rollback, replay),
   `src/state` (ofset tabanlı, işaretçisiz durum bölgesi). D-112 belirlenimcilik sınıfları: sim-kritik = relaxed yok,
   `-ffp-contract=off`, GPU/NaN/float atomik yok, 1T=2T=8T hash; görsel = relaxed/GPU serbest.
3. **Anında yükleme + canlı geliştirme:** küçük wasm, akışlı varlıklar, sıcak yeniden yükleme (shader/sahne/kod).
4. **GPU-sürümlü render:** GPU'da yaşayan sahne, compute kırpma, dolaylı çizim, kümelenmiş aydınlatma (H5).

### 2. Şu anki durum (2026-10-29 itibarıyla; dalga sonuçlarıyla güncelle)
- **wasm build'leri** (D-109, özellik algılama + geri düşüş):
  - `engine.wasm`: SIMD128 + threads/SAB.
  - `engine_relaxed.wasm`: relaxed SIMD.
  - `engine_st.wasm`: tek iş parçacığı, COOP/COEP olmadan çalışır.
  - `web/caps.js` doğru build'i seçer.
- **CPU yazılım render'ı:**
  - Paralel döşeme şeridi binleme, D-105 ertelenmiş g3d, D-106 yalnız-derinlik taraması.
  - PBR, gölge, partikül, mipmap.
  - Sahneler: 1–9 (Sprite, Plasma, Branchy, Editor, 3D, Oyun, Ocak, Kuyu, Okyanus).
- **WebGPU:**
  - RHI komut akışı (`src/gpu/rhi`) + `web/gpu.js`; Matoki→WGSL (M15_A).
  - Matoki compute çekirdekleri F0_A ile geldi: buffer, `global_id`, `atomic_add`; wasm SIMD ve WGSL olarak üretiliyor.
  - `make gpucheck` gerçek GPU'da çalışıyor: headless Chrome + Vulkan, RTX 2060, `tools/host/webgpu_check.mjs`.
- **Okyanus (M17_A):**
  - 3 kademeli Tessendorf FFT, JONSWAP; Hs 2.17 m (teori 2.14 m); yüzen tekne.
  - Ölçüm: CPU 640×360'ta ~11.5 ms. G1_A ile GPU'ya taşınıyor.
- **Fizik:**
  - P1_0: SamurayBalgami/Matoki'den izinle taşınan `src/phys2`. Soft step, GJK/EPA, CCD, eklemler, XPBD, BVH, araç, karakter, DEM.
  - P1_A ile `src/phys` olur. Zayıflık: 8T çözücü ölçeklemesi ~1.1× (P1_B).
- **Altyapı:**
  - ECS çalışma zamanı özelleştiricisi (`src/spec`), kare grafiği v0 (`src/graph`).
  - Girdi olay halkası, iş zamanlayıcı.
  - Belirlenimcilik kanıtı (F0_G): host = Node wasm = Chrome, 1/2/8T aynı hash.
- **Testler ve yayın:**
  - `make check` = build + 300+ test + kural denetimi.
  - GitHub: `Xassemblianist/MatokiEngine` (`/home/ostriquetrum/matoki-wt/queue/publish.sh`, co-author satırlarını siler).
  - Commit e-postası omerkaan20102003@gmail.com.

### 3. İlk işin: dalgayı denetle
- Her son dalga görevi (12 görev) için kontrol et:
  - Durum (`make status`).
  - Notlar'daki iddialar gerçekten kodda/ölçümde var mı? (Argon inceleme/özet yazarken uydurabiliyor; dosya:satır aç-oku.)
  - `make check`, `make detcheck`, `make gpucheck` (+ varsa `make gpucompare`, `make benchphys`).
  - Ekran görüntülerine bak: headless Chrome + Vulkan, 1080p.
- `MERGE_EDILMEDI` / `BLOCKED` kalanları ya bitir ya da yeniden görevlendir.
- Bir "Dalga raporu" yaz (`docs/WAVE2_REPORT.md`):
  - Hipotez tablosu: H1–H12, ölçülen değerler.
  - Kırık/eksik listesi, gerçek performans tablosu.

### 4. Motorun bütün hedefleri (yol haritası; sırayı sen optimize et)

**A. Render (Unreal sınıfı, web'de)**
- Tüm 3D sahneler WebGPU'da, doğal çözünürlükte; CPU yazılım render'ı = geri düşüş + belirlenimci referans.
- GPU-sürümlü boru hattı:
  - Kalıcı örnek tamponları, compute frustum + Hi-Z oklüzyon kırpma.
  - multi-draw-indirect (yoksa geri düşüş), bindless benzeri doku dizileri.
- **Sanal geometri (Nanite benzeri):**
  - Kümelenmiş meshlet LOD DAG, GPU'da küme kırpma.
  - Küçük üçgenler için compute yazılım rasterleştirici (64-bit atomik yoksa paketlenmiş 32-bit görünürlük tamponu).
- **Global aydınlatma (Lumen benzeri):**
  - SDF/voksel tabanlı GI, ekran-uzayı izleme + dünya-uzayı geri düşüş, yansımalar.
  - WebGPU'da donanım ışın izleme yok → compute BVH izleme.
- **Aydınlatma ve malzeme:**
  - Kümelenmiş ileri aydınlatma, CSM + sanal gölge haritaları.
  - PBR, alt yüzey saçılması, saç/kumaş.
- **Atmosfer ve gökyüzü:** Hillaire gökyüzü, hacimsel bulut, hacimsel sis, gün-gece döngüsü.
- **Görüntü kalitesi:**
  - TAA + zamansal üst ölçekleme (FSR2 benzeri, kendi yazdığımız).
  - MSAA seçeneği, anizotropik filtre, sRGB doğru mip'ler.
- **Son işlem:** Bloom, otomatik pozlama, ACES/AgX tonemap, DOF, hareket bulanıklığı.
- **Ölçüm ve testler:**
  - timestamp-query ile her geçişin GPU ms'si HUD'da.
  - CPU↔GPU piksel karşılaştırma testleri.
- **Örnek sahneler:**
  - Okyanus (ufka kadar, köpük, yansıma, kıyı).
  - Prosedürel ada (erozyon, bitki örtüsü).
  - Şehir (100k+ nesne), iç mekân GI demosu.

**B. Matoki dili ve derleyici**
- **IR v2 kararı:** SSA, tipli, çekirdek türleri, alt gruplar (subgroups), paylaşımlı bellek (workgroup), bariyerler.
- Compute: workgroup bellek, alt grup işlemleri, atomikler (u32 + float atomik görsel sınıf), dolaylı dispatch.
- Diferansiyel fuzz (T1) her zaman yeşil; `ir_eval` = kâhin.
- Sıcak yeniden yükleme: Matoki dosyası değişince WGSL + wasm yeniden derlenir, durum korunur.
- Hedef: oyun mantığı da Matoki "system" çekirdeklerinde (ECS sistemleri), CPU JIT'te SIMD + iş parçacıklı.

**C. Fizik (Jolt/PhysX sınıfı, belirlenimci)**
- **Rijit:**
  - Soft step, adalar + uyku, renkli paralel çözücü; 8T'de ≥ 4×; 1T=8T hash.
  - SIMD temas çözücü (skalerle bit-bit aynı).
- **Çarpışma:**
  - GJK/EPA, CCD, mesh/yükseklik alanı.
  - Paralel geniş faz (SAP veya BVH), kanonik çift sırası.
- **Eklemler, araç, karakter:** 6-DOF eklem + motor, ragdoll, araç (raycast + tekerlek modeli), karakter kontrolcüsü (mesh + basamak).
- **Yumuşak cisim:** XPBD kumaş/ip/jöle, iki yönlü rijit bağlaşım, öz-çarpışma, yırtılma.
- **GPU fizik (görsel sınıf):** PBF akışkan 256k+, partikül, yıkım (Voronoi kırılma).
- **Okyanus kaldırma kuvveti:** yüzen cisimler belirlenimci CPU yolunda.
- **Benchmark ve regresyon:** yığın, 10k kutu, kumaş, araç; hash'ler (P1_E).

**D. Belirlenimcilik + ağ**
- **Rollback netcode:** WebRTC DataChannel (güvenilmez, sırasız), girdi tahmini, 8+ kare geri sarma < 1 ms.
- Lockstep/rollback hibrit, desync algılama (her N karede durum hash'i) + otomatik durum farkı raporu.
- **Replay:** girdi kaydı → bit-bit aynı tekrar oynatma; tarayıcılar ve iş parçacığı sayıları arası.
- **Sunucu:** Node'da başsız (headless) aynı wasm ile yetkili sunucu seçeneği.

**E. Çalışma zamanı / platform (web sınırlarını zorla)**
- **Çoklu iş parçacığı:**
  - SAB + Atomics.wait tabanlı iş zamanlayıcı, iş çalma (work stealing).
  - Ana iş parçacığı yalnız sunum (OffscreenCanvas + worker render).
- **Bellek ve hız:**
  - memory64 ölçümü/geçişi (4 GB tavanı).
  - Wasm SIMD + relaxed; JSPI/stack switching varsa coroutine'ler.
- **Varlık yükleme:**
  - Akışlı varlık sistemi: HTTP range + sıkıştırma, GPU doku sıkıştırma (BC/ASTC/ETC2 algılama).
  - OPFS önbellek; anında başlatma (< 1 s ilk kare).
- **Ses:** AudioWorklet + SAB halkası, 3D ses, HRTF, DSP Matoki'de.
- **Girdi:** Gamepad, pointer lock, dokunmatik; mobil bütçe (H12: orta segment mobilde 60 fps).

**F. Araçlar / editör**
- Tarayıcı içi editör: sahne hiyerarşisi, inspector, gizmo, canlı Matoki düzenleme, zaman yolculuğu (rollback ile geri sar).
- Profiler: CPU iş zaman çizelgesi (iş parçacığı başına) + GPU geçiş zamanları + bellek.
- Varlık boru hattı:
  - glTF 2.0 tam (skin, morph, PBR uzantıları), KTX2/Basis.
  - Blender → glTF otomasyonu (Python betikleri).

**G. Vitrin (çığır açıcı demolar)**
- Tek tıkla açılan web demosu: okyanus + ada + gökyüzü + fizik + çok oyunculu rollback.
  Hedef: masaüstü 1440p 60 fps, mobilde ölçekli.
- "Web'de bu yapılamaz" dedirten bir teknik demo: sanal geometri ile milyonlarca üçgenli sahne, GPU akışkan + rijit etkileşim.

### 5. Çalışma biçimi
- **Yönetici sensin; işi kendin yapmak yerine dağıt.**
  - Kuyruk: `tools/agents/` (kopya) / `/home/ostriquetrum/matoki-wt/queue/run2.sh`.
  - Satırlar `items2.txt` içinde: "görev model bağımlılık"; prompt `prompt.txt`.
  - Ajanlar `merge.sh` ile kilit altında kendileri merge + `make check` + push yapar.
  - Ajan modelleri: `gemini-4-argon-high` (görsel/3D/GPU/karmaşık sistem) ve `gemini-3.8-flash-high`
    (dar, mekanik, test, benchmark). Argon'a inceleme/sadakat işi verme; uydurur.
  - Claude ajanı açma (limit).
- Görev dosyası biçimi: `# ID — başlık`, `Durum: TODO`, `## Amaç`, `## Dosyalar`, `## Kabul kriterleri`. Dosya çakışmasını en aza indir.
- Her optimizasyon iddiası ölçümle; makine: 12 iş parçacığı, 15 GB RAM, RTX 2060 + Intel UHD; ölçümde `uptime` yükünü yaz.
- Mimari değişiklik = önce `docs/DECISIONS.md`'ye D-xxx.
- **Arkadaş işbirliği:** SamurayBalgami/Matoki ile birlikte geliştiriyoruz, oradan kod almak serbest.
  - Kaynağı DECISIONS'ta belirt.
  - Bilinen hataları düzelterek al.
  - Repoda LICENSE yok; eklemesini hatırlat.
- Merge sonrası push sormadan yapılabilir (publish.sh). Commit'lerde AI izi kalmasın (publish.sh temizler).
- Kuyu sahnesi (M11_C) park edildi; önceliklendirme.

### 6. Teslim
- **Oturum sonunda:**
  - Güncel `docs/ARCH_V2.md` (hipotez sonuçları, faz durumu) ve `docs/ROADMAP.md`.
  - Bir sonraki dalganın görev dosyaları + `items2.txt`.
  - Kısa Türkçe özet: ne bitti, ne ölçüldü, ne kaldı.
- Kullanıcıya ekran görüntülerini gönder (gerçek GPU, 1080p).
