# Mimari Kararlar (ADR günlüğü)

Bu dosya MATOKI'nin **alınmış** mimari kararlarını ve **bekleyen önerileri** tek yerde tutar.
`ARCHITECTURE.md` anayasadır; buradaki kayıtlar onun ayrıntısı ya da değişikliğidir.
M0–M7_0 girdileri görev dosyalarının Notlar bölümlerinden toplanmıştır (kaynak her girdide).

Biçim: `D-NNN — başlık` · **Durum** (KABUL / ÖNERİ / REDDEDİLDİ / YERİNE GEÇTİ) · Bağlam · Karar · Sonuç.
Yeni karar: dosyanın sonuna sıradaki numarayla ekle; eski girdiyi silme, durumunu güncelle.

---

## A. Temel ve platform

### D-001 — Freestanding C11 → wasm32, libc ve malloc yok
**KABUL** · ARCHITECTURE §0 · M0_1
Motor `clang --target=wasm32 -nostdlib`, `wasm-ld` ile derlenir; Emscripten yok. Tüm bellek `Arena`'dan
(`arena_push` sıfırlanmış döner, taşmada `PANIC`). `src/` altında `#include <...>` yasak (istisna: `*_wasm.c`
içinde `wasm_simd128.h`). Host testleri ve `tools/host/` libc kullanabilir.

### D-002 — Bellek bölgeleri: `perm` 64 MB + `frame` 16 MB
**KABUL** · M0_1
`__heap_base`'den başlayan iki arena; `frame` her `engine_frame` başında sıfırlanır. Alt sistemler
`arena_sub` ile dilim alır (ör. thread başına raster dilimi, D-031).

### D-003 — Bellek ve tablo JS'te oluşturulur, import edilir
**KABUL** · ARCHITECTURE §2 · M0_2, M5_4, M6_1
`env.memory` (256 MB başlangıç, 1 GB max, **shared**) ve `env.__indirect_function_table` JS'te kurulur;
link bayrakları `--import-memory --import-table --growable-table --shared-memory`. Tablonun `initial`
değeri `WebAssembly.Module.imports(mod)`'dan dinamik okunur (en az 64) — sabit `initial: 1` C fonksiyon
işaretçileri yüzünden `LinkError` veriyordu (M2_1 → M5_4'te düzeltildi).

### D-004 — Export'lar `EXPORT(isim)` makrosuyla
**KABUL** · M0_1
`--export-dynamic` kullanılmaz; yalnızca bilinçli export'lar dışarı açılır.

### D-005 — `InputState` JS ile birebir ikili yerleşim
**KABUL** · M0_2, M5_4
`src/plat/input.h`: 256 tuş + fare + delta + `u32 chars[32]; u32 char_count;` (412 byte, `_Static_assert`).
Fare koordinatları `object-fit: contain` hesaba katılarak framebuffer uzayına ölçeklenir.

### D-006 — Framebuffer sunumu: SAB → yerel kopya → `putImageData`
**KABUL** · ARCHITECTURE §2, M5_4, M6_1
`ImageData` paylaşımlı tampon kabul etmez; her kare yerel `Uint8ClampedArray`'e bir kopya yapılır. Bilinçli maliyet.

### D-007 — Geliştirme sunucusu COOP/COEP + `no-store`
**KABUL** · M0_1
`tools/serve.py` port 8080; SharedArrayBuffer için `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`.

### D-008 — Ek platform import'ları yalnızca **ekleme** ile
**KABUL** · M5_4
`plat_download(name, len, ptr, len)` eklendi (F12 PNG indirme). Mevcut import imzaları değişmez.

---

## B. Çekirdek

### D-010 — Kendi transandantal fonksiyonlarımız (libm yok)
**KABUL** · M1_1
`m_sin/cos/tan`: 3 sabitli Cody-Waite (π/2) + minimax; `m_atan2`: oktant + tan(π/8) eşiği;
`exp2/log2/pow`: IEEE üs/mantis ayrıştırma. `sqrt/floor/ceil/trunc/abs` → `__builtin_*` (tek wasm opcode).
Matoki `lower.c` **aynı katsayıları** kullanır → C referansı ile JIT sonuçları aynı yaklaşımı paylaşır.
Ölçülen max hata: sin/cos ≈ 9e-8 mutlak, exp2/log2 ≈ 2e-7 göreli.

### D-011 — Matrisler sütun-öncelikli, perspektif derinlik 0..1
**KABUL** · M1_1
`Mat3/Mat4` column-major; `mat4_perspective` WebGPU/D3D tarzı **[0,1]** derinlik üretir. 3D boru hattı bu konvansiyona uyar.

### D-012 — Sabit nokta: 24.8 (`Fx8`) ve 16.16 (`Fx16`), i64 ara çarpım
**KABUL** · M1_1

### D-013 — RNG: PCG32 (`pcg_onseq_64_xsh_rr_32`)
**KABUL** · M1_1 — referans diziyle birebir doğrulandı.

---

## C. Rasterizer

### D-020 — Tek yol: her şey üçgen; ayrı blit yolu yok
**KABUL** · ARCHITECTURE §3, M2_1
Sprite = 2 üçgen. Tek optimizasyon hedefi.

### D-021 — 24.8 sabit nokta kenar fonksiyonları, piksel merkezi +0.5, top-left kuralı
**KABUL** · M2_1
Bias: `(a > 0) || (a == 0 && b > 0)` → 0, aksi −1. 1000 rastgele ortak kenar testinde 0 boşluk / 0 çift boyama.

### D-022 — 64×64 karo binning, 8×8 blok trivial reject/accept, 2×2 quad gölgeleme
**KABUL** · M2_1, M5_1
Tek karoya düşen üçgen için kesişim testi atlanır (`(tx0^tx1)|(ty0^ty1) == 0`).

### D-023 — Varying interpolasyonu: düzlem denklemi, afin ve perspektif-doğru iki yol
**KABUL** · M2_1, M5_1
`RasterVert.inv_w == 1` → afin; aksi perspektif-doğru. Varyingler SoA `varyings[(v*count+q)*4+lane]`.

### D-024 — Blend: `clamp01 → *255 + 0.5 → trunc`, düz (straight) alpha
**KABUL** · M2_1, M5_1
`BLEND_NONE / ALPHA (src-over) / ADD`. SIMD ve skaler yollar **bit-bit aynı** (256×256 sahnede 0 piksel farkı).
JIT füzyonlu blend de aynı formüle bit-bit uyar (D-043).

### D-025 — Derinlik tamponu `f32`, early-Z
**KABUL** · M5_1
`raster_set_depth(r, test, write, func)` 8 karşılaştırma fonksiyonu. Test shade'den önce yapılır;
yazım `discard` sonrası hayatta kalan lane'ler için.

### D-026 — Dokular 2'nin kuvveti boyutlu, wrap = AND maskesi
**KABUL** · M2_1, `shade_abi.h`

### D-027 — Yerleşik C shader'ları için nöbetçi `ShadeFn` kimlikleri (0xFFFFFFF1..3)
**ÖNERİ (kaldırma)** · M2_1, M5_4
Tablo `initial: 1` sorununu aşmak için konmuştu. D-003 ile tablo boyutu dinamik okunduğundan artık
gereksiz; doğrudan fonksiyon adresleri kullanılabilir. Geriye dönük uyumlu olarak hâlâ kodda.

---

## D. Shader ABI ve Matoki

### D-030 — `ShadeBatch` ABI v1 (sabit ofsetler)
**KABUL** · ARCHITECTURE §4, M5_1
v0 alanlarına `fb_px`, `fb_stride` eklendi (ofset 28, 32); rasterizer **her** batch'te doldurur.
`Texture` 32 byte; `_pad` alanı ileride `levels` (mip seviye sayısı) olarak kullanılmak üzere ayrıldı.

### D-031 — Matoki: SPMD, 2×2 quad = 4 lane, uniform skaler / varying `v128`
**KABUL** · MATOKI.md, M4_1, M4_2
Örtük dönüşüm yok; tüm yardımcı fonksiyonlar inline; özyineleme yasak (3-renk DFS ile tespit).

### D-032 — `ir_eval` bit-bit kâhindir
**KABUL** · M4_2
Referans yorumlayıcı wasm IEEE/SIMD kurallarını (`f32.min/max` NaN ve −0, `nearest` çifte yuvarlama,
`trunc_sat`) birebir uygular. Her `make test`'te 500 rastgele program × (unopt, opt, fused) node'da
çalıştırılıp karşılaştırılır; tolerans **0**.

### D-033 — Uniform özelleştirme ve varyant önbelleği
**KABUL** · M4_2, M5_2
`ir_specialize_uniform_f32` sabit uniform'ları IR'a gömer. `mtk_variant_get`: (kaynak hash, sabit
uniform'lar, codegen seçenekleri, sampler bayrakları) anahtarlı 64 girdilik önbellek.

### D-034 — Codegen: tree-ification + local havuzu + LICM
**KABUL** · M4_2, M5_2
Tek kullanımlı ifadeler yığında kalır; döngüden bağımsız her şey preheader'a. Ölçüm: sprite döngü op'u 191→54.

### D-035 — Örnekleyici seçimi derleme zamanında (`@bilinear @clamp`)
**KABUL** · M4_1
Varsayılan: en yakın + wrap. Sampler bayrakları varyant anahtarına girer.

### D-036 — Relaxed-SIMD kullanılmaz
**KABUL** · M5_3
Donanıma bağlı (x86 FMA/pshufb vs ARM) sonuçlar bit-bit kâhin kuralını (D-032) bozar.

### D-037 — JIT shader'ları karo/batch başına çağrılır
**KABUL** · M3_2
Cross-instance `call_indirect` ≈ +2.25 ns/çağrı (TurboFan); batch başına çağrıldığı için ihmal edilebilir.

---

## E. Kendi araçlarımız

### D-040 — `mbin`: kanonik (en kısa) LEB128 bölüm boyutları
**KABUL** · M3_1
Bölümler önce geçici tampona yazılıp boyut önüne eklenir; çıktı `wat2wasm` ile byte-byte aynı.

### D-041 — `masm`/`mdis`: WAT alt kümesi, `wat2wasm` ile byte-byte, gidiş-dönüş testli
**KABUL** · M3_2, M5_3
31 altın dosya. Desteklenmeyenler: multi-memory, pasif segmentler, `start`/custom bölümler, referans tipleri.

### D-042 — Elle WAT yalnızca gerçek sahnede >%15 kazandırırsa motorda kalır
**KABUL** · ARCHITECTURE §0.5, M5_3, M6_2
Mikro-benchmark yetmez, **gerçek sahne** ölçümü gerekir. Sonuç:
| Çekirdek | Mikro | Gerçek sahne | Karar |
|---|---:|---:|---|
| `mandelbrot_row` | 3.7× | 3.58× | **MOTORDA** |
| `edge_block_8x8` | 1.62× | 0.96–1.11× | çıkarıldı |
| `blend_alpha_rgba8` | 1.72× | 0.98–1.01× | çıkarıldı (füzyonlu blend zaten kapsıyor) |
| `sum_f32`, `dot_f32` | 15×, 9× | 1.02×, 1.05× (adil C ile) | reddedildi — M5_3 iddiası geçersizdi |

### D-043 — Blend füzyonu JIT'te
**KABUL** · M5_2, M5_4
`MTK_FUSE_REPLACE/ALPHA/ADD`: shader RGBA8'e kendisi paketler ve `fb_px`'e yazar; `raster_set_shader_ex(..., fused=TRUE)`
iken C blend aşaması atlanır. Ölçüm: shade+blend 2.7×–4.6× hızlı.

---

## F. Thread'ler

### D-050 — SAB + aynı `engine.wasm`'ı instantiate eden N worker
**KABUL** · ARCHITECTURE §6, M6_1
Worker başına 1 MB yığın + TLS (`engine_thread_init`). Fonksiyon girişinde varargs çağrı yapılmaz
(clang epilogu `__stack_pointer`'ı ezer — M6_2'de bulunan hata).

### D-051 — Kilitsiz MPMC kuyruk (Vyukov) + work-helping
**KABUL** · M6_1
Bekleyen thread kuyruktan iş çeker. `job_parallel_for` atomik indeks çalma.

### D-052 — Worker boştayken 2 ms futex timeout ile JS olay döngüsüne döner
**KABUL** · M6_1, M6_2
Yeni platform import'u gerekmeden JIT modüllerinin worker'lara yayılmasını sağlar.
`jit_load` dönmeden tüm worker'ların ack'ini bekler (`thread_wait_jit_ack`); `job_wake_for_sync` beklemeyi kısaltır.

### D-053 — Paralel raster: karo = iş birimi, dinamik `grain=1`
**KABUL** · M6_2
Thread başına izole `Rasterizer` durumu; karolar ayrık piksellere yazar ve üçgenleri gönderim sırasıyla
işler → 1T ve 8T çıktısı bit-bit aynı. Dinamik çekme statiğe göre %10–33 hızlı.

---

## G. Görüntü ve UI

### D-060 — Kendi PNG kodlayıcı/çözücümüz (DEFLATE sıfırdan)
**KABUL** · M7_0
Satır başına 5 filtre + SAD sezgiseli. 512² mermer: 210 KB (zlib-6: 245 KB).

### D-061 — Kendi 8×8 / 8×16 bitmap fontumuz (ASCII + Türkçe)
**KABUL** · M7_0 — dış font varlığı yok.

### D-062 — Immediate-mode debug UI
**KABUL** · M7_0 — panel, buton, kaydırıcı, grafik, profiler tablosu; kırpma yığını.

### D-063 — Dış varlık (model/doku/font) repoya girmez
**KABUL** · M7_0, M8 planı
Dokular prosedürel, test varlıkları betikle üretilir.

---

## H. Süreç

### D-070 — Ölçmeden optimizasyon iddiası yok; şüpheli sayılar denetlenir
**KABUL** · AGENTS.md, M6_2
M6_2 denetimi M5_3'teki `sum_f32/dot_f32` ve M6_1'deki tek thread Mandelbrot sayılarını düzeltti.
Benchmark tablolarında CPU, tarayıcı ve yöntem (ısınma, medyan) yazılır.

### D-071 — Git ile görev başına branch
**KABUL** · 2026-10-27
Repo git altına alındı. Her görev kendi branch'inde; `make` + `make test` geçince `main`'e merge.
`build/`, `out/`, `web/engine.wasm` izlenmez (yeniden üretilir).

### D-072 — `M8_1_3d` ve `EXTERNAL_3D_KIT` görevleri iptal
**KABUL** · 2026-10-27
Kapsam çok büyüktü (glTF, şehir, gölge…). 3D yine hedef ama **temel** odaklı küçük görevlerle
(boru hattı, kırpma, derinlik, kamera, mipmap). İptal edilen belgeler referans olarak kalır.

---

---

## I. 3D boru hattı (M8_A)

### D-080 — Reverse-Z + sonsuz uzak düzlem, `DEPTH_GREATER`, temizleme 0
**KABUL** · 2026-10-27 · M8_A
`mat4_perspective_rz`: `z_clip = near`, `w_clip = -z_view` → `z/w = near/d` (near'da 1, sonsuzda 0).
f32 derinlikte hassasiyet uzak mesafede korunur, uzak düzlem kırpması gerekmez (uzak nesneler nesne
culling / mesafe ile elenir). Eski `mat4_perspective` (0..1, LESS) 2D/test kodu için kalır.

### D-081 — Kırpma: yakın düzlem gerçek, x/y guard band
**KABUL** · M8_A
Yakın düzlem (`z <= w`) Sutherland–Hodgman ile her zaman kırpılır (w >= near > 0 garantisi).
x/y için yalnızca guard band dışına taşan üçgenler kırpılır; band, raster'ın i32 kenar fonksiyonunun
ara toplamlarda taşmayacağı en büyük kenar payıdır (`clip_guard_margin_px`; 640×360 → 768 px,
1280×720 → 320 px). Kesişim noktası her zaman içerideki köşeden hesaplanır → ortak kenarlar çatlaksız.

### D-082 — Varying perspektif sözleşmesi
**KABUL** · M8_A
Boru hattı `RasterVert.var`'a HAM varying, `inv_w = 1/w_clip` yazar; perspektif-doğru interpolasyonu
raster yapar (`var*inv_w` düzlemi, piksel başına `1/Σ`). Kırpmada varyingler klip uzayında doğrusal
interpole edilir. Kapsanmayan (yardımcı) lane'ler de düzlem denkleminden hesaplanır → quad türevleri
(mipmap) için güvenli.

### D-083 — Arka yüz ayıklama boru hattında, ekran uzayı işaretiyle
**KABUL** · M8_A
NDC'de CCW = ön yüz; ekran (y aşağı) alanı negatif olan tutulur. Raster'ın `cull_back`'i 3D'de kapalı.

### D-084 — Early-Z derinlik yazımı (raster düzeltmesi) + geç yazım modu
**KABUL** · M8_A
Hata: derinlik test anında okunuyor ama batch (≤256 quad) boşaltılınca yazılıyordu → aynı batch'teki
örtüşen üçgenler eski derinliği görüp arka yüzleri sızdırıyordu (2D'de görünmüyordu). Düzeltme:
test geçen lane'lerin derinliği quad üretilirken yazılır. Discard'lı (alfa testli) shader'lar için
`raster_set_depth_ex(..., late_write=TRUE)`: derinlik shade sonrası yalnız hayatta kalan lane'lere
yazılır ve doğruluk için her üçgenden sonra batch boşaltılır (`G3dMaterial.alpha_test`).

### D-085 — Raster çözünürlük sınırı (bilgi)
**ÖNERİ** · M8_A
i32 kenar fonksiyonu ara toplamları 1920×1080'de guard band'siz bile ~2^31'e dayanır (wasm'da sarma
zararsız ama C'de UB). 1080p hedeflenirse: karo-göreli kenar değerlendirmesi (c'yi karo köşesine göre
kurmak) önerilir.

## Bekleyen öneriler (karar verilmedi)

- **Ö-1** `fb_clear`'da paylaşımlı bellekte `memory.fill` yerine her zaman `v128.store` döngüsü
  (Firefox'ta `memory.fill` SAB üzerinde 2× yavaş). — M5_3
- **Ö-2** `simd_clamp01` → `pmin(1, pmax(0, x))`; `simd_pack_rgba8` → `shl 8/16/24 + or` (clang kalıbı, %25–30). — M5_3
- **Ö-3** `asm/*.wat` → `src/bench/asm_sources.c` otomatik gömme Makefile kuralı. — M5_3
- **Ö-4** D-027 nöbetçi kimliklerin kaldırılması.
- **Ö-5 GPU (WebGPU) ikinci arka uç** — 2026-10-27, kullanıcı isteği.
  (a) Matoki IR'ından **WGSL** üreten ikinci codegen (aynı `.mtk` hem CPU hem GPU'da);
  (b) `gfx3d` arka uçtan bağımsız kalır: mesh/materyal/çizim listesi üretir, CPU rasterizer bunun
  bir tüketicisidir, WebGPU arka ucu ikinci tüketici olur;
  (c) aynı kareyi CPU+GPU'ya bölmek yerine **cihaza göre arka uç seçimi** (WebGPU yoksa CPU);
  CPU yolu deterministik referans ve yedek olarak kalır. ARCHITECTURE §0.3 gereği karar bu
  dosyadan geçmeden kodlanmaz.
