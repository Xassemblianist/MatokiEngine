# P1_0 — SamurayBalgami/Matoki fiziğini taşı (src/phys2)

Durum: DONE (Sonnet + Flash raporu; yönetici kapattı)

## Amaç
Arkadaşın motorundaki fizik (`/home/ostriquetrum/matoki-wt/review/SamurayBalgami-Matoki/src/phys/*`, ~6.6k satır;
sahibi kod alınmasına izin verdi) bizim kurallara taşınır. **Yeni dizin `src/phys2/`** (M16_A `src/phys`'e dokunuyor;
çakışmasın — geçiş sonraki adım). İçerik: dünya + soft step çözücü, adalar/uyku, kısıt grafiği renklendirme + SIMD
temas çözücü, dinamik BVH geniş faz (kanonik çift sırası), şekiller + GJK/EPA + CCD + mesh, eklemler, XPBD yumuşak
cisimler, araç, karakter, DEM, okyanus, sorgular (raycast vb.).

## Uyarlama kuralları
- Tipler/arena/isimler bizim `core/` API'mize (`core/types.h`, `core/arena.h`, `core/math.h`; `phys2_` öneki).
  Onların `core/*` yardımcıları gerekirse `src/phys2/` içine özel (static/iç başlık) taşınır; bizim core'a dokunma.
- Onların iş sistemi yerine bizim `thread/job.h` (`job_parallel_for`). Belirlenimcilik: paralel yol seri yolla bit-bit.
- Bilinen hatalar düzeltilir: (1) uyandırılan adalar o adımda düzlem çifti almıyor (`world.c` broadphase: düzlem çiftleri
  uyandırmadan sonra eklenmeli), (2) karakter kontrolcüsü mesh ile çarpışmıyor ve tüm gövdeleri O(N) tarıyor (geniş
  faz sorgusu + mesh testi), (3) çift anahtarı 16 bit → gövde sınırı açıkça denetlenmeli ya da 32+32 bit anahtar.
- Onların testleri `tests/test_phys2_*.c` olarak bizim test çatımıza (`tests/test.h`) taşınır; bit-bit iddiaları
  (SIMD↔skaler, BVH↔SAP) testte kalır. Belirlenimcilik hash testi: aynı sahne 2 koşu + 1T↔8T aynı hash.
- Dosya başlarına kaynak notu: "SamurayBalgami/Matoki'den uyarlandı (izinle)".

## Dosyalar
- `src/phys2/*` (yeni), `tests/test_phys2_*.c` (yeni), `Makefile` (yalnız yeni kaynaklar gerekiyorsa),
  `docs/DECISIONS.md` (D-111: kaynak, uyarlama, düzeltilen hatalar), `tasks/P1_0_phys_port.md`

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız; taşınan testlerin hepsi geçer
- [ ] Düzeltilen 3 hata için ayrı regresyon testi
- [ ] Ölçüm (host -O2 ya da test zamanlaması): kutu yığını / 1000 gövde / kumaş adım süreleri, 1T ve 8T (makine yükünü not et)

## Ara durum raporu (Sonnet durduruldu)

### 1. Yapılan Değişiklikler ve Dosya Dökümü (`git show --stat HEAD` / `git diff main...HEAD --stat`)
- **Son commit:** `65e4f0d` — `P1_0 WIP: Sonnet ajanı limit nedeniyle durduruldu (yarım)`.
- **Fark:** 46 dosya değişikliği, 11.738 ekleme (+), 1 silme (-).
- **Değiştirilen ve eklenen dosyalar:**
  - `docs/DECISIONS.md` (+39 satır): D-111 mimari kararı eklendi (kaynak, uyarlama kuralları, iş parçacığı/belirlenimcilik ilkeleri ve bilinen 3 hata).
  - `tasks/P1_0_phys_port.md`: Durum `IN_PROGRESS (agy)` olarak güncellendi.
  - `src/phys2/*` (17 kaynak dosya: 10 .c, 7 .h):
    - `phys.h`: Ana fizik motoru API'si ve veri yapıları (`Phys2World`, `Phys2Body`, `Phys2Manifold`, `Phys2Joint`, `Phys2RayHit` vb.).
    - `pmath.h`: Fizik matematik yardımcıları (`Phys2Quat`, kuaterniyon fonksiyonları, `phys2_v3_norm`, `Phys2Rng` deterministik xorshift32).
    - `par.h`: Çok çekirdekli iş sistemi paralel parametreleri (`PHYS2_PAR_MIN_PAIRS`, `PHYS2_PAR_CHUNK`).
    - `solver.h`: Kısıt grafiği renklendirme sabitleri (`PHYS2_COLORS = 32`) ve `Phys2WideContacts` arayüzü.
    - `bvh.h` / `bvh.c`: Dinamik AABB ağacı (`Phys2DynamicTree`), SAH maliyetli ekleme, refit, rebalance, ışın ve AABB sorguları.
    - `shapes.h` / `shapes.c`: Geometrik şekiller (Küre, Kutu, Kapsül, Silindir, Koni, Dışbükey Gövde, Üçgen Mesh, Düzlem), eylemsizlik tensörleri, destek fonksiyonları.
    - `collide.c`: İlkel dar faz çarpışma testleri (küre-küre, SAT kutu-kutu vb.) ve mesh çarpışması (`phys2_collide_mesh`).
    - `gjk.h` / `gjk.c`: GJK mesafe/kesişim ve EPA penetrasyon derinliği/vektörü çözücüleri.
    - `joints.h` / `joints.c`: Kısıt eklemleri (küresel, menteşe, prizmatik, sabit, mesafe), warm-starting ve darbe çözücü.
    - `world.c`: Fizik dünyası, Symplectic Euler, yumuşak temas çözücü (compliance soft step), BVH/SAP geniş faz, ada/uyku yönetimi, kanonik çift sıralama, paralel dar faz.
    - `solver_simd_wasm.c`: WASM SIMD128 vektörize temas çözücüsü (skaler çözücüyle bit-bit denk).
    - `query.c`: Işın atma (`phys2_raycast`), AABB sorgusu (`phys2_query_aabb`), şekil süpürme (`phys2_sweep_shape`).
    - `character.h` / `character.c`: Kinematik karakter kontrolcüsü (3 küreli kapsül, basamak tırmanma, eğim algılama, BVH sorgusu ve mesh desteği).
    - `vehicle.h` / `vehicle.c`: 4 tekerlekli süspansiyonlu araç kontrolcüsü, Pacejka benzeri lastik sürtünmesi.
    - `dem.h` / `dem.c`: Ayrık eleman yöntemi (tanecikli/parçacık simülasyonu, Hertz-Mindlin teması).
    - `ocean.h` / `ocean.c`: Tessendorf FFT okyanus dalgaları, Phillips spektrumu, batma/kaldırma kuvveti, paralel FFT.
    - `soft.h` / `soft.c`: XPBD kumaş ve jöle simülasyonu, kısıt renklendirmesi (`phys2_soft_set_colored`), paralel XPBD çözücü.
  - `tests/test_phys2_*.c` (14 dosya):
    - `test_phys2_common.h` / `test_phys2_common.c`: Test arena ve yardımcı rutinler.
    - 12 fonksiyonel test paketi (`test_phys2_bvh.c`, `test_phys2_ccd.c`, `test_phys2_cloth.c`, `test_phys2_dem.c`, `test_phys2_drop.c`, `test_phys2_fall.c`, `test_phys2_game.c`, `test_phys2_ocean.c`, `test_phys2_shapes.c`, `test_phys2_simd.c`, `test_phys2_soft.c`, `test_phys2_world.c`).
    - `test_phys2_port.c`: 3 hatanın regresyon testleri ve belirlenimcilik (1T ↔ çoklu worker, aynı sahne iki koşu).
  - Test ve ölçüm yardımcıları:
    - `tests/test_phys2_wasm.mjs`: Node.js tabanlı WASM test ve benchmark koşucusu (SIMD128 + WebAssembly.Memory shared + Worker Threads).
    - `tests/wasm/phys2_bench.c`: WASM benchmark sahneleri (piramit, kutu yığını, kumaş).
    - `tests/wasm/phys2_shim.c`: WASM test fonksiyonu export shimi.
- **Kaynak repo ile kıyaslama (`/home/ostriquetrum/matoki-wt/review/SamurayBalgami-Matoki/src/phys`):**
  - Kaynak depodaki tüm 17 modül (`bvh`, `character`, `collide`, `dem`, `gjk`, `joints`, `ocean`, `phys`, `pmath`, `query`, `shapes`, `soft`, `solver`, `solver_simd_wasm`, `vehicle`, `world`, `phys_bench`) taşındı.
  - `src/phys/phys_bench.c` dosyası `tests/wasm/phys2_bench.c` altına aktarıldı.
  - Yenilikler: `src/phys2/par.h` (paralel eşikler ve iş sistemi sözleşmesi), `tests/wasm/phys2_shim.c`, `tests/test_phys2_port.c`.
  - Eksik dosya: **YOK**. Kaynaktaki tüm fizik modülleri eksiksiz taşınmıştır.

### 2. `make check` Çalıştırma Sonuçları
- **Derleme Durumu:** WASM motoru (`build/engine.wasm`) ve host test koşucusu (`build/host/test_runner`) `-Wall -Wextra -Werror` altında sıfır hata ve sıfır uyarıyla derlenmektedir.
- **Wasm Doğrulama:** `wasm-validate --enable-all build/engine.wasm` komutu başarıyla geçmektedir.
- **Kural Denetimi:** `src/` altında yasak libc include bulunmamaktadır (`wasm_simd128.h` hariç kural denetimi temiz).
- **Test Koşusu:**
  - Host üzerinde toplam 272 test yer almaktadır.
  - `phys2` testleri: **92/92 testin tamamı firesiz GEÇMEKTEDİR** (`./build/host/test_runner phys2` -> `92 geçti, 0 kaldı`).
  - WASM üzerinde: `node tests/test_phys2_wasm.mjs test 8` ile 8 worker thread altında **92/92 testin tamamı firesiz GEÇMEKTEDİR** (`92 koştu, 0 kaldı`).
  - Tam `make check` koşusu: **272/272 test başarıyla geçmektedir** (`272 geçti, 0 kaldı`, `check: tamam`).
  - *Yarış durumu tespiti (bilgi notu):* Ortamda eşzamanlı olarak birden fazla `test_runner` süreci çalıştığında, `tests/test_jpeg.c:474-475` içerisindeki paylaşımlı `/tmp/matoki_jpeg_bench/` dizinindeki dosyaların yarış halinde silinmesinden dolayı `jpeg_bench_1024_420` testi tekil çakışma yaşayabilmektedir; tek başına çalıştırıldığında (`./build/host/test_runner jpeg`) 7/7 testin tamamı geçmektedir. Temiz tekil `make check` koşusunda sıfır kalma ile tamamlanmaktadır.

### 3. Kabul Kriterleri ve Bilinen 3 Hatanın İncelenmesi (Dosya:Satır Kanıtları)
- **Kriter 1: `make check` hatasız + uyarısız; taşınan testlerin hepsi geçer**
  - **Durum:** **DÜZELTİLDİ**
  - **Kanıt:** Host'ta 272/272 test, `phys2` özelinde 92/92 test, WASM 8 worker ile 92/92 test geçmektedir.
- **Kriter 2: Düzeltilen 3 hata için ayrı regresyon testi**
  - **Durum:** **DÜZELTİLDİ**
  - **Hata 1 — Uyandırılan ada o adımda düzlem çifti almıyordu:**
    - *Durum:* Düzeltildi.
    - *Kaynak hatası:* `/home/ostriquetrum/matoki-wt/review/SamurayBalgami-Matoki/src/phys/world.c:603-637` (düzlem çiftleri ada uyandırmadan önce toplanıyordu; bu adımda uyanan gövde düzlem çifti alamıyordu).
    - *Düzeltme kanıtı:* `src/phys2/world.c:753-765` (düzlem çiftleri uyandırma geçişlerinden SONRA ekleniyor; ayrıca her wake turunun ardından `collect_pairs` yeniden çalıştırılıyor).
    - *Regresyon testleri:* `tests/test_phys2_port.c:68` (`phys2_port_woken_island_gets_plane_pair_bvh`), `tests/test_phys2_port.c:69` (`phys2_port_woken_island_gets_plane_pair_sap`), `tests/test_phys2_port.c:72` (`phys2_port_woken_island_gets_static_pair`).
  - **Hata 2 — Karakter kontrolcüsü mesh ile çarpışmıyor ve tüm gövdeleri O(N) tarıyordu:**
    - *Durum:* Düzeltildi.
    - *Kaynak hatası:* `/home/ostriquetrum/matoki-wt/review/SamurayBalgami-Matoki/src/phys/character.c:52-59` (`for (u32 i = 0; i < w->count; i++)` ile tüm gövdeler doğrusal taranıyordu; `phys_collide` fonksiyonu mesh için 0 dönüyordu).
    - *Düzeltme kanıtı:* `src/phys2/character.c:59-67` (`gather_candidates` fonksiyonu `phys2_query_aabb` + düzlem listesini birleştirerek O(N) yerine mekânsal arama yapıyor); `src/phys2/character.c:90-104` (`b->shape == PHYS2_SHAPE_MESH` kontrolüyle `phys2_collide_mesh` çağrılarak üçgen başına manifold çözülüyor).
    - *Regresyon testleri:* `tests/test_phys2_port.c:102` (`phys2_port_character_stands_on_mesh`), `tests/test_phys2_port.c:116` (`phys2_port_character_blocked_by_mesh_wall`), `tests/test_phys2_port.c:132` (`phys2_port_character_broadphase_not_on` — 500 uzak gövdede sınanan aday sayısı `c.last_tests <= 12`).
  - **Hata 3 — Çift anahtarı 16 bit → 65 536 gövde sınırı ve radix taşması:**
    - *Durum:* Düzeltildi.
    - *Kaynak hatası:* `/home/ostriquetrum/matoki-wt/review/SamurayBalgami-Matoki/src/phys/world.c:34, 345, 610` (`a·2¹⁶ + b` 16 bit paketleme, `query_aabb` 2 radix geçişiyle sınırlıydı).
    - *Düzeltme kanıtı:* `src/phys2/world.c:44` (`pset` tablosu `u64` yapıldı); `src/phys2/world.c:339-360` (`sort_pairs` fonksiyonu `b` ve `a` bileşenleri üzerinde iki aşamalı kararlı radix sıralama uyguluyor, 32+32 bit anahtar eşdeğeri); `src/phys2/world.c:333, 767` (`radix_passes` gövde sayısına göre dinamik geçiş sayısı hesaplıyor).
    - *Regresyon testleri:* `tests/test_phys2_port.c:198` (`phys2_port_pair_keys_support_over_65536_bodies_bvh`), `tests/test_phys2_port.c:199` (`phys2_port_pair_keys_support_over_65536_bodies_sap`) — 65.600 statik gövde ile doğrulanıyor.
- **Kriter 3: Ölçüm tablosu (1T ve 8T adım süreleri, makine yükü):**
  - **Durum:** **YARIM** (Ölçüm altyapısı `tests/wasm/phys2_bench.c` ve `tests/test_phys2_wasm.mjs bench` içinde eksiksiz kuruldu ve çalıştırılabilir durumda; ancak görev dosyasındaki kabul kriterleri kutucukları işaretlenmeden ve ölçüm tablosu görev dosyasına eklenmeden önceki ajan Sonnet durduruldu).
  - **Doğrulanan Benchmark Ölçüm Sonuçları:**
    - *Ortam:* Node 22, V8 WASM SIMD128, 12 çekirdekli Linux x86_64, sistem yükü (`load average`): ~4.5 - 5.0.
    | Senaryo | 1T (ms) | 8T (ms) | Paralel Hızlanma / Açıklama |
    |---|---|---|---|
    | Piramit (650 kutu, oturmuş adım medyan) | 5.288 | 4.881 | Dar faz: 1.649 ms → 1.011 ms (1.63x) |
    | 1000 kutu yığını (oturmuş adım) | 2.374 | 2.204 | Dar faz: 0.828 ms → 0.560 ms (1.48x) |
    | 1000 kutu yığını (skaler çözücü, SIMD kapalı) | 4.035 | 3.822 | SIMD (1T): 2.374 ms vs Skaler (1T): 4.035 ms (1.70x SIMD hızlanması) |
    | 2000 kutu yığını (oturmuş adım) | 5.079 | 4.615 | Dar faz: 1.793 ms → 1.128 ms (1.59x) |
    | Kumaş 100x100 (dizi sırası, öz-çarpışma kapalı) | 13.151 | 13.305 | Seri sıra zorunlu (paralel değil) |
    | Kumaş 100x100 (renk sırası, öz-çarpışma kapalı) | 13.257 | 7.992 | **1.66x hızlanma** (renk grupları paralel) |
    | Kumaş 200x200 (dizi sırası, öz-çarpışma kapalı) | 56.042 | 56.500 | Seri sıra |
    | Kumaş 200x200 (renk sırası, öz-çarpışma kapalı) | 69.672 | 30.893 | **2.25x hızlanma** (paralel XPBD çözücü) |
    | Kumaş 100x100 (renk sırası, öz-çarpışma AÇIK) | 38.262 | 31.565 | 1.21x hızlanma |
    | Kumaş 32x32 (renk sırası, öz-çarpışma kapalı) | 1.318 | 1.326 | Küçük iş yükü (overhead nedeniyle tekil) |

### 4. Kalan İşin Sıralı Listesi
1. `tasks/P1_0_phys_port.md` dosyasındaki kabul kriterleri kutularının (`[x]`) işaretlenmesi ve yukarıdaki benchmark tablosunun görev Notlar bölümüne eklenmesi.
2. `tasks/P1_0_phys_port.md` durumunun `DONE` olarak güncellenmesi.
3. Çoklu test süreçlerinde rastlanan `tests/test_jpeg.c` içindeki `/tmp/matoki_jpeg_bench/` yarış durumunun düzeltilmesi için ileride izole geçici dizin kullanılması (ayrı refactor notu).
4. `P1_0_phys_port` dalının `main` dalına merge edilmesi.
5. M16_A görevinin tamamlanmasının ardından `src/phys2/` kodunun `src/phys/` yerine taşınarak nihai geçişin yapılması (D-111).
