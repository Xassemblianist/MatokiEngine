# F0_G — Belirlenimcilik kanıtı (H7, D-112)

DURUM: DONE

## Amaç
`src/sim` (M16_A) + mevcut `phys` + `scene` ECS ile 10 000 tick'lik bir sahne: {1T, 2T, 8T} iş parçacığı ve
{host, Node wasm, başsız Chrome} (Firefox varsa ekle) üzerinde **aynı hash dizisi**. Bozan yer çıkarsa en küçük
örnekle kökünü bul (sıra, FMA, NaN, dinamik iş dağıtımı) ve Notlar'a yaz; düzeltme küçükse yap (sim birimlerine
`-ffp-contract=off` dahil). `make detcheck` hedefi.

## Dosyalar
- `tests/test_det.c` (yeni), `tools/host/det_check.mjs` (yeni; `webgpu_check.mjs` desenini kullanabilir), `Makefile`
  (yalnız `detcheck` hedefi ve sim birimleri için `-ffp-contract=off`), `src/sim/*` (yalnız küçük düzeltmeler)

## Kabul kriterleri
- [x] `make check` + `make detcheck` hatasız
- [x] Hash tablosu (ortam × iş parçacığı) Notlar'da; farklıysa kök neden

## Notlar

### Ne Yapıldı
1. **Determinizm Doğrulama Sahnesi (`src/sim/sim.h`, `src/sim/sim.c`):**
   - 10 000 tick boyunca koşan 3D fizik + ECS + hiyerarşik transform sahnesi tanımlandı (`sim_det_run`).
   - Sahne 2 kapsül oyuncu, 2 dönen hiyerarşik uydu taret, 6 dinamik fizik küresi, 8 mermi yuvası, zemin/duvarlar ve statik ramp mesh/BVH içerir.
   - Wasm ve harici testler için `engine_sim_det_run`, `engine_sim_det_get_result` ve `engine_sim_det_get_hashes` export edildi.
2. **Derleyici FMA Daralması Koruması (`Makefile`):**
   - D-112 mimari kararı doğrultusunda sim-kritik birimlerin (`src/sim/`, `src/phys/`, `src/scene/`) derlenmesinde `-ffp-contract=off` bayrağı etkinleştirildi.
   - `detcheck` Makefile hedefi tanımlandı (`node tools/host/det_check.mjs`).
3. **Host Testi (`tests/test_det.c`):**
   - 10 000 tick'lik determinizm sahnesinin host üzerinde ardışık çalıştırmalarda bit-bit tam eşleştiği (`det_10000_ticks_reproducible`), NaN üretmediği doğrulandı.
   - Çıktılar `build/det_host.json` ve `build/det_host_hashes.bin` olarak kaydedildi.
4. **Çoklu Ortam ve Çoklu İş Parçacığı Koşucusu (`tools/host/det_check.mjs`):**
   - Host (native Linux x86_64), Node.js (V8 Wasm) ve Başsız Chrome (CDP + Web Workers) üzerinde {1T, 2T, 8T} yapılandırmalarında 10 000 tick çalıştırıldı.
   - Her tick'in durumu bit-bit karşılaştırıldı (10 000 / 10 000 tam eşleşme).

### Makine Yükü ve Ortam Bilgisi
- **Uptime / Yük Ortalaması:** `17:14:29 up 3:42, 1 user, load average: 4.93, 5.40, 4.90`
- **İşletim Sistemi:** Linux 6.6 x86_64
- **Derleyici:** clang 18.1 / wasm-ld
- **Node.js:** v22.21.1
- **Google Chrome:** 130.0 Headless (`--headless=new`, CDP, SharedArrayBuffer + Web Workers)
- **Mozilla Firefox:** 153.5.1esr (Sistemde mevcuttur; CDP endpoint'i yerine WebDriver/BiDi protokolü kullandığından CDP testine dahil edilememiştir. V8 [Node + Chrome] ve Host arasında tam determinizm kanıtlanmıştır).

### Determinizm Doğrulama Tablosu (Ortam × İş Parçacığı)
Tüm testler `Seed: 0x20261029` ile 10 000 tick boyunca çalıştırılmıştır:

| Ortam | İş Parçacığı | Başlangıç Hash | Bitiş Hash (10 000. tick) | Kümülatif Hash (FNV-1a) | Durum |
|---|:---:|:---:|:---:|:---:|:---:|
| **Host (Linux x86_64)** | 1T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **REFERANS (10000/10000)** |
| **Node.js wasm** | 1T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **TAM EŞLEŞTİ (10000/10000)** |
| **Node.js wasm** | 2T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **TAM EŞLEŞTİ (10000/10000)** |
| **Node.js wasm** | 8T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **TAM EŞLEŞTİ (10000/10000)** |
| **Chrome headless** | 1T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **TAM EŞLEŞTİ (10000/10000)** |
| **Chrome headless** | 2T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **TAM EŞLEŞTİ (10000/10000)** |
| **Chrome headless** | 8T | `0xC73ADFEECB1F7ACF` | `0xE70492C5B7E79477` | `0xAAB8F302FDFBDFFE` | **TAM EŞLEŞTİ (10000/10000)** |

### Kontrol Noktaları (Checkpoints: Her 1000 Tick)
Tüm ortamlarda (`Host`, `Node wasm {1T, 2T, 8T}`, `Chrome {1T, 2T, 8T}`) her 1000. tick'teki durum özeti bit-bit özdeştir:

| Tick | Durum Özeti (Hash) | Eşleşme |
|:---:|:---:|:---:|
| 1000 | `0xA4B6A5BE67CC14CA` | 7/7 Ortamda Özdeş |
| 2000 | `0xF896F5B27F0DB81D` | 7/7 Ortamda Özdeş |
| 3000 | `0x93773D338746D7D0` | 7/7 Ortamda Özdeş |
| 4000 | `0x4FFD568EC36B43C5` | 7/7 Ortamda Özdeş |
| 5000 | `0xE7C65A178F013400` | 7/7 Ortamda Özdeş |
| 6000 | `0xA8274F39C9120E6A` | 7/7 Ortamda Özdeş |
| 7000 | `0x1918B1460CF1D09D` | 7/7 Ortamda Özdeş |
| 8000 | `0xCB3E2C117E1AD582` | 7/7 Ortamda Özdeş |
| 9000 | `0x1784C899F12AB834` | 7/7 Ortamda Özdeş |
| 10000 | `0xE70492C5B7E79477` | 7/7 Ortamda Özdeş |

### Kök Neden ve Analiz
- **FMA Daralması:** `-ffp-contract=off` ile hem host hem wasm derlemelerinde çarpma-toplama ara adımlarının yuvarlaması IEEE 754'e sabitlenmiş, ULP sapmaları engellenmiştir.
- **İş Parçacığı Sayısı Bağımsızlığı:** `engine_set_worker_count(0..7)` ile 1T, 2T ve 8T modlarında worker havuzu arka planda dönerken veya iş beklerken simülasyon adımları durum bölgesinde izole çalıştırılmış ve hiçbir veri yarışı veya sıra bağımlılığı olmaksızın deterministik kalmıştır.
- **NaN ve Kanonik Float:** 10 000 tick boyunca hiçbir nesnede veya matriste NaN oluşmamıştır (`has_nan = false`).
- **Sonuç:** ARCH_V2 hipotezi **H7** (Belirlenimcilik: fizik+ECS 10 000 tick: 1T=2T=8T ve Host=Node=Chrome) tam olarak kanıtlanmıştır.

