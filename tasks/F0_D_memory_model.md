# F0_D — Bellek modeli ölçümleri (H4)

DURUM: DONE

## Amaç
ARCH_V2 §2 bellek modeli için sayılar. Tarayıcıda (wasm, paylaşımlı bellek) ve host'ta ölç:
`memory.copy` vs SIMD döngü vs `__builtin_memcpy` (1–64 MB), 16 MB durum bölgesi anlık görüntüsü, 8 iş parçacığıyla
paralel kopya, durum özeti (hash) hızı, `memory.grow` maliyeti, 4 GB'a yakın bellek davranışı; memory64'ün Chrome'da
bayrakla durumu (yalnız rapor).

## Dosyalar
- `tools/host/bench_memory.c` (yeni), `web/mem_bench.html` + `web/mem_bench.js` (yeni), `src/bench/bench_memory.c` (yeni)
- `Makefile` (yalnız yeni hedefler)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Host sonuçları tablo halinde Notlar'da; tarayıcı sayfası tek tıkla aynı tabloyu üretir
- [x] H4 eşiği (16 MB < 2 ms) için ön değerlendirme

---

## Notlar

### Ortam ve Makine Durumu
- **İşlemci:** Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz (12 mantıksal çekirdek)
- **Makine Yükü (Uptime / Load Avg):** `1 dk: 3.84, 5 dk: 4.02, 15 dk: 3.82` (paylaşımlı ortam)
- **Derleyici / Platform:** Clang 18 (`-O1 -fsanitize=address,undefined` host, `-O2 -nostdlib -msimd128 -mbulk-memory -matomics` wasm32)
- **Tarayıcı:** Google Chrome 131 (Headless / V8) + Web Workers + SharedArrayBuffer (COOP/COEP)

---

### 1. Host Sonuçları (`tools/host/bench_memory.c` / `make bench_mem`)

#### 1.1 Kopyalama Kıyaslaması (Tek İş Parçacığı: memcpy vs SIMD vs Skaler u64)

| Boyut | Yöntem | Medyan (ms) | Min (ms) | Bant Genişliği (GB/s) |
|---|---|---:|---:|---:|
| 1 MB | libc memcpy | 0.040 | 0.039 | 25.17 |
| 1 MB | SIMD loop | 0.219 | 0.208 | 4.70 |
| 1 MB | skaler u64 | 0.325 | 0.319 | 3.07 |
| 4 MB | libc memcpy | 0.258 | 0.209 | 18.71 |
| 4 MB | SIMD loop | 0.552 | 0.492 | 7.93 |
| 4 MB | skaler u64 | 0.803 | 0.742 | 5.26 |
| 16 MB | libc memcpy | 1.521 | 1.239 | 12.61 |
| 16 MB | SIMD loop | 2.810 | 2.506 | 6.23 |
| 16 MB | skaler u64 | 3.855 | 3.341 | 4.68 |
| 64 MB | libc memcpy | 7.589 | 5.729 | 10.91 |
| 64 MB | SIMD loop | 13.142 | 11.407 | 5.48 |
| 64 MB | skaler u64 | 16.203 | 15.284 | 4.09 |

*Gözlem:* `libc memcpy` non-temporal streaming store ve mimariye özel mikro-kod optimizasyonları sayesinde skaler ve elle yazılmış SIMD döngülerini 2×–5× kat geçmektedir.

#### 1.2 16 MB Durum Bölgesi Snapshot & H4 Değerlendirmesi
- **16 MB Snapshot Medyan:** `1.521 ms`
- **16 MB Snapshot Min:** `1.239 ms` (`12.61 GB/s`)
- **H4 Eşik Kriteri (16 MB < 2.0 ms):** **GEÇTİ (DOĞRULANDI)**. 60 FPS kare bütçesi olan 16.6 ms içinde 16 MB durum bölgesinin tam kopyalanması yalnızca ~1.2–1.5 ms (%7–9 bütçe) tutmaktadır.
- **8 Kare Halka Tamponu (128 MB) Kayıt + 1 Rollback Geri Yükleme:** Toplam `52.966 ms` (Kare başına ortalama ~`5.8 ms`).

#### 1.3 Paralel Kopya (1T, 2T, 4T, 8T Pthread)

##### Hedef Boyut: 16 MB
| İş Parçacığı | En İyi Süre (ms) | Bant Genişliği (GB/s) | Hızlanma |
|---|---:|---:|---:|
| 1T | 1.438 | 10.87 | 1.00x |
| 2T | 1.534 | 10.19 | 0.94x |
| 4T | 1.282 | 12.18 | 1.12x |
| 8T | 1.677 | 9.32 | 0.86x |

##### Hedef Boyut: 64 MB
| İş Parçacığı | En İyi Süre (ms) | Bant Genişliği (GB/s) | Hızlanma |
|---|---:|---:|---:|
| 1T | 6.991 | 8.94 | 1.00x |
| 2T | 6.171 | 10.13 | 1.13x |
| 4T | 5.815 | 10.75 | 1.20x |
| 8T | 6.168 | 10.13 | 1.13x |

*Gözlem:* Tek çekirdek zaten DDR4 bellek bant genişliğini doyurmaktadır (~11–12 GB/s). 8 iş parçacığı çalıştığında thread oluşturma ek yükü ve bellek veri yolu çekişmesi nedeniyle 4T üstünde ek hızlanma görülmemektedir (64 MB'da en yüksek verim 4T ile 1.20x).

#### 1.4 Durum Özeti (Hash - XXH64) Hızı

| Boyut | Süre (ms) | Bant Genişliği (GB/s) | Özet Hash |
|---|---:|---:|---|
| 1 MB | 0.166 | 5.90 | `0x4896255F65C84916` |
| 4 MB | 0.734 | 5.33 | `0x2BC26763A6F162D6` |
| 16 MB | 2.936 | 5.32 | `0x3ACA423F59C39F33` |
| 64 MB | 12.881 | 4.85 | `0x409A8200471D2F77` |

*Gözlem:* 16 MB durumun tam hash'ini almak ~2.9 ms sürmektedir. Her kare tüm durumun hash'lenmesi yerine, karesel delta hash (yalnız kirli aralıklar) veya seyrek/strided hash yapılması önerilir.

#### 1.5 Bellek Büyütme Maliyeti (mmap Sayfa Ayırma)
- 64 KB (1 wasm sayfası): `0.7 µs`
- 1 MB (16 sayfa): `3.5 µs`
- 16 MB (256 sayfa): `3.9 µs`

#### 1.6 4 GB'a Yakın Bellek Davranışı (Host)
- 4 GB `mmap` tahsisi anında başarılı (`0.020 ms`).
- 4 GB sınır adresine (son 1 KB) erişim ve okuma/yazma hatasız doğrulandı.

---

### 2. Tarayıcı Sonuçları (`web/mem_bench.html` + `web/mem_bench.js` / Google Chrome)

Sayfa tek tıkla (`Tüm Ölçümleri Çalıştır` butonu ve sayfa açılışında otomatik) aynı tabloyu üretmektedir:

#### 2.1 Wasm Kopyalama Kıyaslaması (memory.copy vs SIMD128 vs __builtin_memcpy)

| Boyut | Yöntem | Medyan (ms) | Min (ms) | Bant Genişliği (GB/s) |
|---|---|---:|---:|---:|
| 1 MB | `__builtin_memcpy` | 0.039 | 0.029 | 32.55 |
| 1 MB | `memory.copy` | 0.035 | 0.034 | 27.90 |
| 1 MB | SIMD128 loop | 0.070 | 0.054 | 17.75 |
| 4 MB | `__builtin_memcpy` | 0.349 | 0.339 | 11.48 |
| 4 MB | `memory.copy` | 0.425 | 0.320 | 12.20 |
| 4 MB | SIMD128 loop | 0.714 | 0.574 | 6.79 |
| 16 MB | `__builtin_memcpy` | 2.389 | 1.744 | 8.95 |
| 16 MB | `memory.copy` | 1.634 | 1.465 | 10.66 |
| 16 MB | SIMD128 loop | 2.665 | 2.330 | 6.70 |
| 64 MB | `__builtin_memcpy` | 7.089 | 6.029 | 10.36 |
| 64 MB | `memory.copy` | 6.429 | 5.745 | 10.87 |
| 64 MB | SIMD128 loop | 10.250 | 9.355 | 6.68 |

*Gözlem:* Tarayıcıda wasm bulk-memory `memory.copy` opcode'u, SIMD128 döngüsünden belirgin şekilde hızlıdır (~%60–80 daha yüksek bant genişliği). Büyük boyutlarda (16–64 MB) `memory.copy` en tutarlı düşük gecikmeyi vermektedir.

#### 2.2 Tarayıcıda 16 MB Snapshot ve H4 Değerlendirmesi
- **16 MB Snapshot Medyan:** `1.964 ms`
- **16 MB Snapshot Min:** `1.605 ms` (`9.73 GB/s`)
- **H4 Eşik Kriteri (16 MB < 2.0 ms):** **GEÇTİ (H4 DOĞRULANDI)**.

#### 2.3 Web Worker Havuzu ile Paralel Kopya (1T, 2T, 4T, 8T)
- 16 MB: `1T: 2.090 ms` -> `2T: 1.399 ms (1.49x)` -> `4T: 1.610 ms (1.29x)` -> `8T: 1.459 ms (1.43x)`
- 64 MB: `1T: 6.910 ms (9.04 GB/s)` -> `2T: 5.370 ms (1.28x)` -> `4T: 4.740 ms (13.18 GB/s, 1.45x)` -> `8T: 5.064 ms (1.36x)`

#### 2.4 Wasm memory.grow Maliyeti
- 1 sayfa (64 KB): `15.0 µs`
- 16 sayfa (1 MB): `0.0 µs` (rezerv sanal alanda anında genişleme)
- 256 sayfa (16 MB): `615.0 µs`

#### 2.5 4 GB'a Yakın Bellek Davranışı (Tarayıcı / V8)
- `new WebAssembly.Memory({ initial: 65536, maximum: 65536, shared: true })` başarıyla oluşturuldu (4,294,967,296 bayt).
- 4 GB sınır erişimi (`buffer[4294967295]`) doğrulanmıştır.
- 65537 sayfa istendiğinde V8 anında `Property 'initial': value 65537 is above the upper bound 65536` hatası vererek tavanı zorlamaktadır.

#### 2.6 memory64 Chrome Bayrak Durumu Raporu
- **Durum:** Chrome'da `--js-flags="--experimental-wasm-memory64"` bayrağı ile memory64 modülleri (`i64` bellek adresleme) geçerli kabul edilmektedir (`WebAssembly.validate` doğrulandı).
- **Standart:** W3C Phase 3.
- **Mimari Değerlendirme:** MATOKI L0 için 4 GB sınırı ileride kaldırılabilecektir; 32-bit ofsetli veri modeli (ARCH_V2 §2) sayesinde 64-bit işaretçilerin bellek şişirme riski motora yansımaz. Faz 0 için varsayılan olarak `wasm32 + 1 GB SAB` yeterli ve en uyumlu modeldir.
