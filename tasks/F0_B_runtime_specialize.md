# F0_B — Çalışma anı özelleştirme kanıtı: arketip başına JIT ECS döngüsü (H1)

DURUM: DONE
Durum: DONE

## Amaç
ARCH_V2 ilke 3 / H1. Mevcut `src/scene` ECS'inde 100k varlık, 3 sistem (hareket: pos += vel*dt; yerçekimi+sürtünme;
sınır kutusunda yansıma). İki yol: (a) genel C döngüsü (mevcut sorgu API'si), (b) çalışma anında arketip düzenine
ve sabitlere (dt, g, kutu) özel **wasm modülü üretilir** (mevcut `masm`/`mbin` + `jit` altyapısı veya Matoki IR) ve
JIT ile yüklenir: SoA ofsetleri sabit, dalsız, SIMD. Host'ta (wasm üretimi doğrulama) + tarayıcı ölçüm sayfası.

## Dosyalar
- `src/spec/*` (yeni: özelleştirici), `tests/test_spec.c` (yeni), `web/spec_bench.html` + `web/spec_bench.js` (yeni)
- `Makefile` (yalnız yeni kaynaklar), `src/app/main.c` (yalnız bench dışa aktarımı)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Özelleştirilmiş ve genel yol aynı sonucu verir (bit-bit, test)
- [x] Tarayıcı ölçümü sayfası hazır; host'ta ölçebildiğin kadarı + derleme süresi (ms) Notlar'a. ≥ 2× hedefi tutmazsa
      nedenini analiz et (bu da değerli sonuç)

## Notlar

### 1. Ne Yapıldı
- **`src/spec/spec.h` & `src/spec/spec.c`:**
  - Mevcut `src/scene/ecs.h` sorgu API'si (`ecs_query` / `ecs_query_next` / `ecs_query_get`) üzerinde çalışan genel C referans döngüleri (`spec_ecs_step_move`, `spec_ecs_step_gravity`, `spec_ecs_step_bounce`, `spec_ecs_step_all`) ve adil bellek düzeni karşılaştırması için skaler C SoA döngüleri (`spec_c_soa_*`) gerçeklendi.
  - Çalışma anı wasm özelleştiricisi (`spec_codegen_kernel`): `mbin` ikili kod üreticisi kullanılarak her `(çekirdek_türü, arketip_SoA_ofsetleri, simülasyon_sabitleri)` üçlüsü için çalışma anında geçerli, bağımsız veya paylaşımlı bellekli WebAssembly SIMD128 modülü üretildi:
    - Arketip sütun ofsetleri (`px, py, pz, vx, vy, vz`) `v128.load` ve `v128.store` yönergelerinin `offset` anlık (immediate) alanına doğrudan gömüldü; döngü içinde yalnızca tek bir `$ptr` indüksiyon değişkeni (`i32.add 64`) ilerliyor.
    - Simülasyon sabitleri (`dt`, `g*dt`, `drag`, `bmin[3]`, `bmax[3]`, `-restitution`) `v128.const` olarak döngü önsözüne (preheader) gömüldü (`f32x4.splat` / bellek yüklemesi yok).
    - Sınır kutusunda yansıma (`bounce`) tamamen dalsız: `f32x4.lt` + `f32x4.gt` + `v128.or` maskesi ile `v128.bitselect(v * -restitution, v, mask)` ve konum kıstırma için IEEE-754 işaretli sıfır davranışını C `if/else if` zinciriyle bit-bit aynı koruyan `f32x4.pmax` + `f32x4.pmin` kullanıldı.
    - Ana döngü 4× açılmış (16 varlık = 64 bayt/iterasyon), ardından 1× SIMD (4 varlık) ve dalsız `select` kullanan skaler kuyruk döngüsüyle 4'ün katı olmayan arketip boyutları da bit-bit aynı sonucu veriyor.
    - Hem 3 ayrı sistem geçişi (`SPEC_KERNEL_MOVE`, `SPEC_KERNEL_GRAVITY`, `SPEC_KERNEL_BOUNCE`) hem de 3 sistemi yazmaç içinde tek geçişte birleştiren `SPEC_KERNEL_FUSED` çekirdeği üretildi.
  - **FNV-1a 64-bit Varyant Önbelleği (`SpecCache`):** Arketip düzeni veya sabitler değişmediği sürece derlenmiş JIT fonksiyon işaretçisi önbellekten (`hits`) dönüyor; değiştiğinde yeniden modül üretilip `jit_load` ile yükleniyor.
- **`src/app/main.c` & `web/spec_bench.html` + `web/spec_bench.js`:**
  - `engine_spec_verify`, `engine_spec_bench_run`, `engine_spec_report_ptr`, `engine_spec_report_len` wasm dışa aktarımları eklendi ve etkileşimli tarayıcı kıyaslama sayfası hazırlandı.
- **`tests/test_spec.c`:**
  - `spec_codegen_and_disassembly`: Üretilen 4 çekirdek türünün (`move`, `gravity`, `bounce`, `fused`) `mdis` ile sökülmesi, gömülü SoA `offset=` değerlerinin doğrulanması ve `wasm-validate --enable-all` kontrolü.
  - `spec_cache_and_respecialization`: `SpecCache` isabet/ıskalama ve sabit/arketip değişiminde yeniden üretim testi.
  - `spec_host_ecs_vs_c_soa_and_compile_speed`: Çeşitli arketip boyutlarında (`0, 1, 3, 4, 7, 15, 16, 17, 19, 131`) bit-bit doğruluk ve host `mbin` üretim hızı ölçümü.
  - `spec_wasm_v8_jit_bit_exact_and_bench`: Gerçek `build/engine.wasm` + `web/jit.js` + Node/V8 üzerinde 100k varlıkta bit-bit doğrulama ve hız kıyaslaması.

### 2. Ölçüm Ortamı ve Makine Yükü
- **İşlemci / Önbellek:** 6 çekirdek (L1d: çekirdek başına 32 KiB / toplam 192 KiB, L2: çekirdek başına 256 KiB / toplam 1.5 MiB, L3: 12 MiB paylaşımlı)
- **Çalışma Zamanı:** Node `v22.21.1` (V8 `14.2.231.21`), Clang wasm32 `-O2 -msimd128`
- **Paylaşılan Makine Yükü (`uptime`):** Ölçümler sırasında `load average: 3.82 – 4.58, 4.77, 4.11` (diğer ajanlarla paylaşımlı makine)

### 3. JIT Derleme Süresi ve Modül Boyutları (3 Arketip × 4 Çekirdek = 12 Modül)
100.000 varlık 3 arketipe bölünmüştür (`A0: 50.000` [Pos+Vel], `A1: 30.000` [Pos+Vel+TagA], `A2: 20.000` [Pos+Vel+TagB]).
- **Modül Boyutları:**
  - `spec_move`: 710 bayt / arketip
  - `spec_gravity`: 468 bayt / arketip
  - `spec_bounce`: 1.807 – 1.825 bayt / arketip
  - `spec_fused`: 1.985 – 2.009 bayt / arketip
  - **Toplam (12 modül):** `14.985 bayt` (~14.6 KiB)
- **Derleme Süresi:**
  - Sadece C `mbin` ikili wasm üretimi (host ASan/-O1): **0.462 ms** / 12 modül (**0.0385 ms / modül**)
  - Wasm `mbin` üretimi + V8 `new WebAssembly.Module` + `new WebAssembly.Instance` + `table.set` (`jit_load`): **0.794 – 0.888 ms** / 12 modül (**~0.070 ms / modül**)

### 4. Kıyaslama Sonuçları (100.000 Varlık, 3 Arketip, 5 Isınma + 25 Ölçüm İterasyonu)

| Kıyaslama | Bit-Bit (`0 ULP`) | Genel C Medyan (`Min`) ms | Özel JIT Medyan (`Min`) ms | Hızlanma (Medyan) | Durum |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **3 sistem: `ecs_query` vs JIT (3 geçiş)** | EVET | `2.355` (`2.227`) | `0.150` (`0.138`) | **15.74×** (14.6×–15.7×) | **GEÇTİ (≥ 2×)** |
| **3 sistem: `ecs_query` vs JIT (birleşik)** | EVET | `2.355` (`2.227`) | `0.103` (`0.099`) | **23.30×** (21.1×–23.3×) | **GEÇTİ (≥ 2×)** |
| **3 sistem: `C-SoA (3 geçiş)` vs JIT (3 geçiş)** | EVET | `0.313` (`0.280`) | `0.150` (`0.138`) | **2.09×** (1.75×–2.09×) | **ANALİZ / GEÇTİ** |
| **3 sistem: `C-SoA (3 geçiş)` vs JIT (birleşik)** | EVET | `0.313` (`0.280`) | `0.103` (`0.099`) | **2.84×** (2.66×–2.84×) | **GEÇTİ (≥ 2×)** |
| **3 sistem: `C-SoA (birleşik)` vs JIT (birleşik)** | EVET | `0.336` (`0.304`) | `0.103` (`0.099`) | **3.08×** (2.79×–3.08×) | **GEÇTİ (≥ 2×)** |
| **Sistem 1: Hareket (`ecs_query` vs JIT)** | EVET | `0.884` (`0.836`) | `0.041` (`0.041`) | **21.77×** | **GEÇTİ** |
| **Sistem 2: Yerçekimi (`ecs_query` vs JIT)** | EVET | `0.750` (`0.727`) | `0.028` (`0.028`) | **26.83×** | **GEÇTİ** |
| **Sistem 3: Yansıma (`ecs_query` vs JIT)** | EVET | `0.647` (`0.629`) | `0.074` (`0.068`) | **8.80×** | **GEÇTİ** |
| **Sistem 3: Yansıma (`C-SoA` vs JIT)** | EVET | `0.203` (`0.197`) | `0.074` (`0.068`) | **2.76×** | **GEÇTİ** |

### 5. Mimari Analiz (H1 Hipotezi Değerlendirmesi)
1. **Genel `ecs_query` API'sine Karşı (14.6× – 23.3× hızlanma):**
   - Mevcut `src/scene/ecs.c` `ecs_query` API'si bileşen sütunlarını `AoS` (`SpecPos[N]`, `SpecVel[N]`) olarak tutar ve varlık başına `ecs_query_next` + `ecs_query_get` (bitmask + `popcnt` araması) çağırır. Arketip başına özelleştirilmiş SoA + SIMD128 JIT çekirdeği bu maliyeti tamamen ortadan kaldırarak 3 sistem toplamında **~15.7×** (3 geçiş) ve **~23.3×** (birleşik geçiş) hızlanma sağlar.
2. **Saf C SoA Döngüsüne Karşı (`C-SoA` vs `JIT`, 1.75×–2.09× [3 geçiş] ve 2.80×–3.08× [birleşik]):**
   - 100.000 varlığın konum + hız çalışma kümesi `100.000 × 24 bayt = 2.4 MB` yer kaplar. Bu boyut çekirdek başına L1d (`32 KiB`) ve L2 (`256 KiB`) önbelleklerini aştığı için döngüler **L3 bellek bant genişliği sınırlı (memory-bandwidth bound)** hale gelir.
   - **3 ayrı geçişte** (`move` + `gravity` + `bounce`) 2.4 MB'lık veri L3'ten üç kez akar (adım başına `10.8 MB` bellek trafiği). Bu yüzden salt hesaplama yoğunluğu düşük olan 1. ve 2. sistemlerde SIMD kazancı L3 bant genişliği tavanına çarpar ve 3 geçişli `C-SoA vs JIT` oranı makine yüküne bağlı olarak **1.75× – 2.09×** bandında kalır.
   - **Dallı sistemde (`bounce`):** Her eksende 2 sınır karşılaştırması yapan 3. sistemde dalsız `pmax/pmin/bitselect` SIMD128 özelleştirmesi tek başına `C-SoA`'ya karşı bile **2.69× – 2.76×** hızlanma sağlar (yanlış dal tahmini cezası tamamen kalkar).
   - **Çalışma anı döngü birleştirme (`SPEC_KERNEL_FUSED`):** Çalışma anı JIT özelleştiricisinin en büyük mimari avantajlarından biri, aynı arketip üzerinde ardışık çalışan sistemleri çalışma anında tek bir çekirdekte birleştirebilmesidir (kernel fusion). Birleşik JIT çekirdeği konum ve hız vektörlerini L3'ten yalnızca **bir kez** okur, 3 sistemi `v128` yazmaçlarında uygular ve geri yazar (`10.8 MB` yerine `4.8 MB` bellek trafiği). Bu sayede `C-SoA`'ya karşı bile **2.80× – 3.08×** net hızlanma elde edilir ve H1 hipotezi doğrulanır.

