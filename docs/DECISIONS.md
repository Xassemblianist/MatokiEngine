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

## I. 3D boru hattı (M8_A, M8_B)

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

### D-086 — Mip zinciri: bitişik, küçük kenarda biter, kapalı form taban ofseti
**KABUL** · 2026-10-27 · M8_B
`Texture._pad` → `levels` (ABI boyutu/ofsetleri değişmedi, `TEX_OFF_LEVELS = 28`), `TEX_FLAG_MIPMAP = 4`.
Seviyeler `px`'ten bitişik; `levels <= 1 + min(log2 w, log2 h)`. Bu sınır sayesinde seviye k tabanı
`(4wh - (4wh >> 2k)) / 3` kapalı formuyla (3'ün mod 2^32 tersiyle çarpma) IR'da döngüsüz hesaplanır.
Bedeli: kare olmayan dokularda en küçük birkaç seviye yok (256×16 → 5 seviye, 9 değil).

### D-087 — Türevler kaba (coarse), seviye seçimi tamsayı, yardımcı lane'ler çalışır
**KABUL** · M8_B
`IR_DERIV_X/Y` (enum sonuna eklendi) = `a[lane1|lane2] - a[lane0]`, 2 shuffle + 1 sub. Mip seviyesi
`rho2`'nin f32 üs bitlerinden (`>> 23`, `-127`, `>> 1`); `rho2 >= 1` değilse (NaN dahil) 0 →
C referansı ile bit-bit. Türev içeren shader'larda varying `if` atlaması kapsama maskesine değil 4 lane'e
göre yapılır (codegen: `loc_active` başta tüm-1; ir_eval aynısı). Aksi halde atlanan bölgelerden
kapsanmayan lane'lere çöp değer girer ve türev bunu kapsanan lane'lere taşır — diferansiyel test bunu
yakaladı (opt öncesi/sonrası farklı sonuç).

### D-085 — Raster çözünürlük sınırı (bilgi)
**ÖNERİ** · M8_A
i32 kenar fonksiyonu ara toplamları 1920×1080'de guard band'siz bile ~2^31'e dayanır (wasm'da sarma
zararsız ama C'de UB). 1080p hedeflenirse: karo-göreli kenar değerlendirmesi (c'yi karo köşesine göre
kurmak) önerilir.

---

## J. 2D temeli ve ses (M7_1..M7_3)

### D-090 — Ses: C'de synth, paylaşılan bellekte SPSC halka tampon, AudioWorklet yalnız kopyalar
**KABUL** · 2026-10-27 · M7_2
Tüm ses üretimi `src/audio/` (C). `engine_audio_init(sample_rate)` → `AudioRing*`; ana thread her
karede `audio_pump(0.07 s)` ile halkayı hedef gecikmeye kadar doldurur. Worklet wasm belleğinin
SharedArrayBuffer'ından doğrudan okur ve `read_pos`'u Atomics ile ilerletir → postMessage/kopya yok.
JS'te mantık yok (ARCHITECTURE §0.2). AudioContext ilk kullanıcı etkileşiminde başlar (tarayıcı kuralı).
Bilinen sınır: render ana thread'de; >70 ms'lik kare takılması ses kesintisi yapar (worklet `underruns` sayar).

### D-091 — Synth: polyBLEP kare/testere, 15-bit LFSR gürültü, yumuşak kırpma, deterministik
**KABUL** · M7_2
Aynı girdi → örnek-örnek aynı çıktı (test). Karışım rasyonel tanh yaklaşımı + [-1,1] kırpma.

### D-092 — 2D karo çarpışması: eksen ayrık, yarım-karo alt adımlı
**KABUL** · M7_1
`tilemap_move` önce x sonra y; adım ≤ 0.45 karo → tek karede 20 karo hızda bile tünelleme yok.
Tek yönlü platform: yalnız aşağı inerken ve önceki alt kenar karo üstündeyse katı. Rastgele harita
fuzz testi (10k+ hareket): AABB hiçbir zaman katı karoya girmiyor.

### D-093 — Pixel-art: tamsayı zoom + piksel hizalı kamera + tek atlas
**KABUL** · M7_3
Kamera ötelemesi tam piksele yuvarlanır (titreşim ve karo arası aralık olmaz); en yakın komşu örnekleme
karo sınırlarından taşmaz. Batch2D doku değişiminde tüm raster'ı boşalttığı için sahne başına tek atlas
(256×256, 16 px) kullanılır; gökyüzü ve paralaks katmanları ayrı (3 doku değişimi/kare).

### D-094 — Oyun fiziği sabit 1/120 s adım
**KABUL** · M7_3
Kare hızından bağımsız, tekrarlanabilir (coyote time, zıplama tamponu, değişken zıplama yüksekliği).

---

## K. Varlık hattı (M8_C, M8_D)

### D-095 — Vertex aşaması: SoA klip konumu, talep üzerine ClipVert
**KABUL** · 2026-10-27 · M8_C
Konum dönüşümü SoA ve wasm'da f32x4 (skalerle bit-bit aynı sıra). 80 baytlık `ClipVert` artık her köşe
için değil yalnız kırpılan üçgenlerin köşeleri için kurulur. Tamamen görünür nesnede outcode atlanır.
`g3d_vertex` 1.25 → 0.78 ms. Thread'li ölçümler kareler arasında bekleyerek alınır (art arda çağrı worker'ları aç bırakır).

### D-096 — Kendi JSON / OBJ / glTF ayrıştırıcılarımız
**KABUL** · M8_D
JSON: RFC 8259, arena DOM, son anahtar kazanır, derinlik 256, sayılar Clinger hızlı yolu (tam) +
1e22 parçalı yedek (ölçülen hata: tipik aralıkta ≤1 ULP, ±1e320'de ≤4 ULP). glTF: accessor erişimleri
u64 ile sınır denetimli; skin/morph/animasyon/sparse/JPEG/uzantılar uyarı (zorunlu uzantı = hata).
Dokular PNG (kendi çözücümüz) → 2'nin kuvvetine (≤1024) bilinear boyut → mip zinciri.
Test varlıkları `tests/assets/make_assets.py` ile üretilir (D-063).

### D-097 — Sürükle-bırak: ayrı asset arena + kareler arası yükleme
**KABUL** · M8_D
`engine_file_begin/alloc/add/end` export'ları; dosya baytları ve model 48 MB `g_asset_arena`'da
(her grupta sıfırlanır), geçici bellek kareler arasında frame arena. JS yalnız bayt kopyalar (`web/filedrop.js`).

## L. Işık ve gölge (M9_A)

### D-098 — Raster yalnız-derinlik kipi: `shade_fn == NULL`
**KABUL** · 2026-10-27 · M9_A
Shader'sız çizimde quad kuyruğa girmez; derinlik testi + early-Z yazımı emit sırasında yapılır,
shade/blend hiç çalışmaz. Gölge haritası geçişi böyle çizilir (skaler ve SIMD emit yollarında aynı).

### D-099 — Gölge haritası: ortografik reverse-Z, 24 bit RGB kodlu derinlik
**KABUL** · M9_A
Güneş için tek gölge haritası (1024²), kameranın önündeki 36 birimlik küreye oturtulur; izdüşüm
merkezi texel ızgarasına hizalanır (kamera dönerken titreme yok). Gölge düşürenler ön yüz ayıklamalı çizilir.
Matoki dokuları RGBA8 olduğundan derinlik `q = round(d·(2^24−1))` → R,G,B baytlarına kodlanır; shader
`r·0.99609 + g·0.00389 + b·0.0000152` ile çözer, 2×2 texel çift doğrusal ağırlıklı karşılaştırma (PCF) yapar.
Sapma: 1.5 texel'lik dünya mesafesi (derinlik birimine çevrilmiş). Bedeli: kodlama 1M piksel/kare (~1.5 ms) — bkz. Ö-10.

### D-100 — PBR modeli (pbr3d.mtk)
**KABUL** · M9_A
Metal/pürüzlülük; GGX dağılımı, Schlick-GGX görünürlük (k = (r+1)²/8), Schlick Fresnel (x⁵ çarpımla),
yarım küre ortam ışığı + metaller için kaba yansıma payı; doğrusal uzayda sis; ACES (Narkowicz) ton eşleme;
sRGB yerine gama 2 (giriş c², çıkış √c — pow yok). Gece-gündüz: güneş yörüngesi, ufukta turuncu ışık,
gece ay (soluk mavi, güneşin karşısı). C referansı `ref_shade_pbr3d` aynı formüller.

## M. Oyun katmanı (M10_A)

### D-101 — app/r3d: sahneden bağımsız 3D kare çizimi
**KABUL** · 2026-10-27 · M10_A
Sahne bir `R3dSceneFn` verir; `r3d_render` onu iki kez çağırır (gölge geçişi, ana geçiş). Nesneler üç türdür:
`r3d_draw` (gölge düşürür + ışık alır), `r3d_draw_receiver` (yalnız ışık/gölge alır, ör. zemin),
`r3d_draw_emissive` (unlit3d.mtk; gölge düşürmez, ışıktan etkilenmez). Gün ışığı modeli `r3d_daylight(tod)`.

### D-102 — Üçüncü şahıs kamera çarpışması
**KABUL** · M10_A
Hedeften göze `phys_world_raycast`; engel varsa kamera anında öne çekilir, engel kalkınca üstel yumuşatmayla
(≈6/s) geri açılır. Ölçüt: başlangıç sahnesinde kameranın 6 m'lik sütunun 0.4 m dışında kalıp ekranı kaplaması.

### D-103 — Retro FPS ("Kuyu"): zindan ışıklandırması ve fare kilidi
**KABUL** · M11_A
- `dungeon3d.mtk`: gölgesiz, 4 nokta ışık (pencereli `(1-d²/r²)²/(1+d²/4)` sönüm, yarım-Lambert) + sabit ortam + sis;
  doku en yakın komşu + mipmap. Işıklar CPU'da çizim başına seçilir: 4×4 karoluk seviye parçası veya nesne
  kutusuna en çok katkı veren 4 ışık (meşale, namlu alevi, mermi, patlama). Gölge haritası geçişi yok → ucuz.
- Sahne 320×180'e çizer, `retro_present` ile 640×360'a büyütür (15 bit + Bayer + tarama çizgisi; V ile kapatılır).
- Silah modeli ayrı geçişte (derinlik temizlenir) çizilir: duvara gömülmez.
- Fare kilidi JS sözleşmesine isteğe bağlı iki export olarak eklendi (ARCHITECTURE §2); kilitliyken
  `mouse_dx/dy` ham `movementX/Y` taşır. Sahne değişince JS kilidi bırakır.

### D-104 — Köşe yakalama boru hattında, düşman yol bulma akış alanıyla
**KABUL** · M11_B
- `G3dContext.snap` (> 0 ise ekran köşeleri 1/snap piksel ızgarasına `retro_snap_xy` ile yuvarlanır); `g3d_begin`
  sıfırlar, `g3d_set_snap` açar. Hem kırpılmamış (SIMD) hem kırpılmış köşe yolunda uygulanır. Ölçülen maliyet gürültü içinde.
- Kuyu düşmanları karo ızgarasında oyuncudan BFS akış alanı izler (yer + hava için iki alan); A* yerine akış alanı
  çünkü tüm düşmanların hedefi aynı ve harita küçük (≤ 36×18).

### D-105 — Paralel 3D ön uç: ertelenmiş çizim + g3d_flush
**KABUL** · M13_B
- `g3d_set_deferred(ctx, max_draws)` açıkken `g3d_draw` yalnız culling yapar, matris/materyal kopyasıyla komut
  kaydeder ve raster durumunu kayıtta sabitler (`raster_pin_state`). `g3d_flush` köşe aşamasını (4096 köşelik
  işler) ve üçgen aşamasını (1024 üçgenlik işler: outcode, kırpma, arka yüz + `raster_setup_tri`) `job_parallel_for`
  ile çalıştırır; kurulmuş üçgenler iş parçacığı dilimlerinden (frame arena'dan sıfırlamasız `arena_sub`) gönderim
  sırasıyla `raster_bin_tris` ile karolara dağıtılır. Karo dağıtımı (bin) ana iş parçacığında kalır.
- Çıktı anlık yolla **bit düzeyinde aynı** (renk, derinlik, istatistik; test `gfx3d_deferred_matches_immediate`).
  Dilimi yetmeyen iş ana iş parçacığında kesin kapasiteyle (üçgen × 7) yeniden işlenir (`items_fallback`).
- Workers içinde PROF ve frame arena kullanılmaz. `arena_push_raw` (sıfırlamasız) eklendi; `arena_sub` artık dilimi
  memset etmez (alt arenanın kendi ayırmaları zaten sıfırlanır).
- Gölge haritası kodlaması 16384 piksellik işlere bölündü.

## N. GPU arka ucu (M15)

### D-108 — WebGPU ikinci arka uç (Ö-5 kabul), RHI komut akışı
**KABUL** · 2026-10-29, kullanıcı kararı ("çığır açıcı WebGPU + WebAssembly motoru")
- Motor mantığı C/wasm'da kalır. GPU erişimi **RHI** (`src/gpu/`) üzerinden: C tarafı kaynak (buffer, doku,
  sampler, pipeline, bind group) tutamaçları ve kare başına **komut akışı** (paylaşımlı bellekte düz, sürümlü ikili
  biçim) üretir; `web/gpu.js` bu akışı kare başına bir kez WebGPU çağrılarına çevirir. JS'te karar/mantık yok
  (AGENTS.md JS kuralları); akış biçimi ARCHITECTURE §2'de tanımlanır.
- Shader'lar tek kaynak: aynı `.mtk` → wasm (CPU JIT) ve **WGSL** (Matoki IR'ından ikinci codegen).
- Arka uç seçimi cihaza göre: WebGPU varsa GPU, yoksa CPU rasterizer. CPU yolu deterministik referans ve yedek
  olarak kalır; GPU çıktısı CPU çıktısıyla tolerans içinde karşılaştırılarak test edilir.
- `gfx3d` arka uçtan bağımsız kalır (mesh + materyal + çizim listesi); WebGPU arka ucu ikinci tüketicidir.

### D-109 — Platform sınırlarını zorla: en yeni web özellikleri, tespit + yedek
**KABUL** · 2026-10-29, kullanıcı kararı ("katı web prensiplerini esnetebildiğimiz kadar esnet; CPU da GPU da çok iyi")
- Motor, tarayıcının sunduğu en yeni ve en hızlı özellikleri varsayılan olarak kullanır: SharedArrayBuffer + wasm
  thread'leri (COOP/COEP zorunlu), SIMD128 ve **relaxed SIMD**, bulk memory, WebGPU (compute, timestamp-query,
  subgroups, f16 — varsa), OffscreenCanvas ile worker'da render, Atomics.waitAsync, WebCodecs/WebTransport/WebRTC.
- Her özellik açılışta **tespit edilir** (yetenek raporu wasm'a verilir); yoksa bir alt basamağa düşülür (ör. relaxed
  SIMD yoksa SIMD128 derlemesi, WebGPU yoksa CPU raster). "En düşük ortak payda" için tasarım yapılmaz; tavan hedeflenir,
  taban yalnız çalışır kalır.
- Birden çok wasm derlemesi (ör. `engine.wasm` + `engine_relaxed.wasm`) kabul edilir; JS yükleyici yeteneğe göre seçer
  (seçim kuralı ARCHITECTURE §2'de, JS'te başka mantık yok).
- CPU ve GPU yolları birinci sınıftır: CPU yolu yalnız "yedek" değil, kendi başına en hızlı hale getirilir.

## O. Belirlenimci simülasyon ve ağ (M16)

### D-110 — Deterministik simülasyon çekirdeği, tek arena snapshot halkası ve rollback
**KABUL** · 2026-10-29 · M16_A
- **Sabit adımlı döngü ve girdi halkası (`src/sim/`):** Simülasyon varsayılan 60 Hz (`1/60 s`) sabit adımla ilerler.
  Oyuncu girdisi 16 baytlık dolgusuz tamsayı yapısıdır (`SimInput`: `buttons`, `move_x/y`, `look_dx/dy`, `aux`);
  oyuncu başına 256 karelik halka tamponda (`SimInputSlot`) kare numarası ve `confirmed` bayrağıyla tutulur.
- **Tahmin ve geri sarma (rollback):** Kare `f` simüle edilirken oyuncu `p`'nin gerçek girdisi henüz gelmemişse
  `f - 1` karesindeki girdi tahmin olarak tekrarlanır (`confirmed = FALSE`). Sonradan `f < current_frame` için
  gerçek girdi ulaştığında tahminden farklıysa en eski kirli kare (`rollback_frame`) işaretlenir; o karenin
  başlangıç anlık görüntüsü geri yüklenip `current_frame`'e kadar yeniden simüle edilir.
- **Tek arena bölgesi ve ofset/indeks sözleşmesi:** Oyun durumu tek bir bitişik `Arena` bölgesinde (`sim->state`,
  `sim->state_size`) tutulur; anlık görüntü (snapshot) bu bölgenin 2'nin kuvveti uzunluklu (varsayılan 16 kare)
  halka tampona düz `memcpy` kopyasıdır. Durum içindeki çapraz referanslar ham C işaretçisi (`T*`) yerine dizi
  indeksi (`u32`), `EcsEntity` tutamağı veya bölge tabanından bayt ofseti (`SimOffset` / `SIM_PTR`) kullanır;
  böylece snapshot'lar taşınabilir ve ham durum özeti (`sim_hash`) adres-bağımsız olur. Mevcut `PhysWorld` ve
  `EcsWorld` yapılarını durum arenası içinde kullanan sahneler için işaretçi adreslerini atlayıp yalnız değer
  dizilerini ve kanonik float'ları (`-0.0f → +0.0f`, tüm NaN → tek kanonik qNaN) özetleyen `sim_hash_phys` ve
  `sim_hash_ecs` yardımcıları sunulur.
- **Belirlenimcilik denetimi düzeltmeleri:** `phys/shapes.h` içindeki `shape_from_*` kurucularında `PhysShape`
  birliğinin (union) kullanılmayan kuyruk baytları sıfırlandı (yığın çöpünün durum özetini bozması önlendi);
  `scene/transform.c` içinde `quat_norm` NaN korumalı hale getirildi ve `walk_stack` temizlendi; `scene/ecs.c`
  içinde silinen yoğun dizi yuvaları, serbest liste ve ertelenmiş komut tamponu sıfırlanarak arena belleğinin
  geçmiş çöp veriden bağımsız kalması sağlandı.

## Bekleyen öneriler (karar verilmedi)

- **Ö-1** `fb_clear`'da paylaşımlı bellekte `memory.fill` yerine her zaman `v128.store` döngüsü
  (Firefox'ta `memory.fill` SAB üzerinde 2× yavaş). — M5_3
- **Ö-2** `simd_clamp01` → `pmin(1, pmax(0, x))`; `simd_pack_rgba8` → `shl 8/16/24 + or` (clang kalıbı, %25–30). — M5_3
- **Ö-3** `asm/*.wat` → `src/bench/asm_sources.c` otomatik gömme Makefile kuralı. — M5_3
- **Ö-4** D-027 nöbetçi kimliklerin kaldırılması.
- **Ö-5 GPU (WebGPU) ikinci arka uç** — 2026-10-27, kullanıcı isteği. → **D-108 ile KABUL edildi (2026-10-29).**
  (a) Matoki IR'ından **WGSL** üreten ikinci codegen (aynı `.mtk` hem CPU hem GPU'da);
  (b) `gfx3d` arka uçtan bağımsız kalır: mesh/materyal/çizim listesi üretir, CPU rasterizer bunun
  bir tüketicisidir, WebGPU arka ucu ikinci tüketici olur;
  (c) aynı kareyi CPU+GPU'ya bölmek yerine **cihaza göre arka uç seçimi** (WebGPU yoksa CPU);
  CPU yolu deterministik referans ve yedek olarak kalır. ARCHITECTURE §0.3 gereği karar bu
  dosyadan geçmeden kodlanmaz.
- **Ö-6** Batch2D'de doku değişimi tüm raster'ı boşaltıyor (`raster_flush`); çok dokulu 2D sahneler
  için doku başına durum (RasterShaderState zaten doku işaretçisi taşıyor) ile boşaltmasız geçiş. — M7_3
- **Ö-7** Ses render'ını bir worker'a taşımak (ana thread takılmalarına dayanıklılık). — M7_2
- **Ö-8** JSON sayıları için tam doğru yuvarlama (Eisel-Lemire veya büyük tamsayı yedeği) — şu an ≤4 ULP. — M8_D
- **Ö-9** glTF: çoklu primitive başına ayrı materyal varyantları, normal haritası/PBR (`pbr.mtk`), KTX/JPEG. — M8_D
- **Ö-10** Matoki'ye f32 doku biçimi (`texture2d_f32` / `sample_depth`): gölge haritası derinliği kodlamadan
  doğrudan okunur, ~1.5 ms/kare kodlama ve shader'daki çözme kalkar. — M9_A
- **Ö-11** Matoki'yi shader dilinden **genel sistem diline** büyütmek (kullanıcı isteği, 2026-10-27): struct, dizi,
  doğrusal belleğe işaretçi, arena ile bellek, açık SIMD tipleri, çöp toplayıcı ve çalışma zamanı yok; oyun mantığı da
  Matoki ile yazılır ve JIT ile derlenir. Önce ölçüm: Matoki çıktısı ile elle yazılmış en iyi WAT aynı shader'larda. — M9_A
- **Ö-12** Retro 3D modu (kullanıcı isteği): düşük iç çözünürlük + tamsayı büyütme, en yakın komşu doku, isteğe bağlı
  köşe yakalama (vertex snapping) ve renk derinliği azaltma/titreşim (dither) — eski konsol estetiği, yazılımsal raster'da ucuz. — M9_A
