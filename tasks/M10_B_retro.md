DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M10_B — Retro görüntü modu (eski konsol estetiği), sıfırdan

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turdaki incelemelerde bozuk/düşmanca girdiler bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle.

## Hedef
Düşük iç çözünürlükte çizilmiş kareyi eski konsol havasıyla ekrana basmak: tamsayı büyütme, renk derinliği azaltma
(ör. 15 bit / 5:5:5), sıralı (Bayer 4×4) titreşim, isteğe bağlı palet eşleme (≤256 renk, en yakın renk için 32³ arama
tablosu), tarama çizgisi (scanline) karartma, isteğe bağlı CRT benzeri hafif yatay bulanıklık. Ayrıca 3D için **köşe
yakalama (vertex snapping)** yardımcısı: ekran koordinatlarını piksel ızgarasına (veya 1/N alt piksele) yuvarlayan saf fonksiyon
(entegrasyonu ben yapacağım).

## Dosyalar (YALNIZCA bunlar)
- `src/gfx3d/retro.h/.c` — `RetroParams` (iç w/h, ölçek, bit derinliği, dither açık/kapalı, palet, scanline gücü),
  `retro_present(const Framebuffer* src, Framebuffer* dst, const RetroParams*)`, `retro_build_palette_lut`,
  `retro_snap_xy(f32* x, f32* y, u32 subpixel)`; isteğe bağlı `retro_wasm.c` (SIMD, skalerle bit-aynı)
- `tests/test_retro.c`
- `tasks/M10_B_retro.md`

## Kabul
- [x] `make check` hatasız/uyarısız (ASan/UBSan)
- [x] Tamsayı büyütme: her kaynak pikseli tam s×s blok; hedef kaynaktan büyük/küçük/ölçeğe bölünmüyorsa ortalanır ve kenar temizlenir, sınır dışı yazım yok
- [x] 5:5:5 + Bayer: düz gri rampada ortalama hata ≤ 1/2 LSB(5 bit); dither kapalıyken çıktı bit-bit tahmin edilebilir (tablo testi)
- [x] Palet: 16 ve 256 renkli paletlerde LUT sonucu, kaba kuvvet en yakın renkle 32³ örnekte birebir aynı
- [x] `retro_wasm.c` yazıldıysa skalerle bit-aynı (rastgele 1000 kare)
- [x] Benchmark (host + not): 320×180 → 1280×720 present süresi (ms) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/gfx3d/pipeline.*`, `src/raster/*`, `src/app/*` değişmez. Öneriler Notlar'a.

## Notlar

### Yapılanlar
- **Nicemleme ve Bayer 4×4 titreşim (`src/gfx3d/retro.h`, `src/gfx3d/retro.c`)**:
  - `color_bits` hem kanal başına bit (`1..8`, ör. `5`) hem toplam RGB bit (`9=3:3:3`, `12=4:4:4`, `15/16=5:5:5`, `24/32=8:8:8`) olarak kabul edilir.
  - Dither kapalıyken `q = c >> (8 - bpc)` ve bit tekrarı (`5:5:5` için `(q5 << 3) | (q5 >> 2)`) ile genişletilir; tablo testiyle 256 seviyenin tamamı bit-bit doğrulandı.
  - Dither açıkken 4×4 Bayer matrisi (`0..15`) ile tam bölmesiz özdeşlik `q = ((c * max_q + bayer * 16) * 32897) >> 23` (= `floor((c * max_q + bayer * 16) / 255)`) kullanıldı. Bu formül `c = 0` için tüm 16 Bayer hücresinde tam `0`, `c = 255` için tam `255` üretir.
  - 0..255 düz gri rampada ölçülen 5:5:5 + Bayer 4×4 ortalama mutlak hata (`1 LSB(5 bit) = 255/31 ≈ 8.2258`, `1/2 LSB ≈ 4.1129`):
    - **4×4 blok ortalaması hatası**: **0.2957 / 255** (**0.0359 LSB** ≤ 0.5 LSB)
    - **Tekil piksel ortalama hatası**: **2.7341 / 255** (**0.3324 LSB** ≤ 0.5 LSB)
- **Palet eşleme (`retro_build_palette_lut`)**:
  - `RetroPalette` yapısı `≤256` RGBA8 renk ve `32×32×32 = 32768` baytlık `(r5, g5, b5) → palet indeksi` arama tablosu (`lut`) tutar (toplam ~33 KB, L1 önbelleğe sığar).
  - 16 ve 256 renkli rastgele paletlerde `32³ = 32768` noktanın tamamında LUT sonucu kaba kuvvet (`retro_palette_nearest_brute`) ile **32768/32768 birebir aynı**.
- **Tamsayı büyütme, ortalama, kırpma, scanline ve CRT yatay bulanıklık (`retro_present`)**:
  - Tüm nicemleme, Bayer titreşimi, palet araması, `[1, 6, 1]/8` yatay CRT bulanıklığı ve tek satır scanline karartması kaynak çözünürlükte (`src_w`) işlenir; hedef satırlara `s` çarpanıyla genişletilir ve aynı `s×s` blok içindeki eş çift/tek satırlar `__builtin_memcpy` ile kopyalanır.
  - `src == dst` (yerinde) kullanımda satırlar sondan başa işlenir ve kenar temizliği sonradan yapılır; veri ezilmesi olmaz.
  - Düşmanca girdilere karşı korumalar: `NULL` işaretçiler, `0` veya `> RETRO_MAX_DIM` (`16384`) boyutlar, `scale = U32_MAX` (64-bit `i64` merkezleme ve yalnız hedefle kesişen `[sx_start, sx_end)` / `[sy_start, sy_end)` aralığını tarama sayesinde sonsuz/uzun döngü oluşmaz), `NaN`/`±Inf` `scanline`, `> 2048` genişlikte yığın taşmasız yedek yol ve kanarya (`0xCAFEBABE`) sınır denetimi.
- **3D köşe yakalama (`retro_snap_xy`)**:
  - Ekran koordinatlarını `m_floor(v * sub + 0.5f) / sub` ile `1/sub` alt piksel ızgarasına yuvarlar (`subpixel == 0` → `1`, `> 256` → `256`, `NaN` → `0.0f`, `≥ 8388608` taşma korumalı).
- **Wasm SIMD (`src/gfx3d/retro_wasm.c`)**:
  - `retro_quantize_row`, `retro_blur_row`, `retro_darken_row` ve `retro_expand_row` (`s = 2, 3, 4` shuffle genişletmesi) `i32x4` SIMD ile yazıldı; tamamen tamsayı aritmetiği kullanıldığından skaler `retro.c` ile **1000 rastgele karede 0 piksel farkı** (bit-bit aynı) doğrulandı.

### Benchmark (320×180 → 1280×720, `scale = 4`)
- **Donanım / Ortam**: Intel Core i7-10750H @ 2.60 GHz, Clang 19.1.7, Node.js v22.21.1 (V8 wasm SIMD).
- **Yöntem**: Isınma sonrası host `test_runner`'da 31 tekrar medyanı; `-O2` karşılaştırmasında 10 ısınma + 100 tekrar ortalaması.

| Kip (320×180 → 1280×720, `s=4`) | Host Skaler (`-O1` + ASan/UBSan) | Host Skaler (`-O2`) | Wasm32 SIMD (`-O2 -msimd128`) |
|---|---:|---:|---:|
| 5:5:5 + Bayer 4×4 (`scanline = 0`) | — | 0.289 ms | **0.242 ms** |
| 5:5:5 + Bayer 4×4 + Scanline (`0.25`) | 0.954 ms | 0.338 ms | **0.319 ms** |
| 5:5:5 + Bayer 4×4 + Scanline + CRT Blur | 1.357 ms | 0.582 ms | **0.417 ms** |
| 256 Renk Palet + Bayer + Scanline + CRT Blur | 1.755 ms | 0.764 ms | **0.487 ms** |

### Öneriler / Entegrasyon Notları
- `pipeline.c` içinde `project()` fonksiyonunda ekran koordinatları üretildikten hemen sonra (veya 24.8 sabit noktaya geçmeden önce) `retro_snap_xy(&o.x, &o.y, snap_subpixel)` çağrılarak PS1 tarzı köşe titremesi (vertex snapping) doğrudan etkinleştirilebilir.
- Sahne düşük iç çözünürlüklü bir `Framebuffer`'a (ör. `320×180`) çizildikten sonra `engine_frame` sonunda `retro_present(&low_fb, &main_fb, &retro_params)` ile ana `Framebuffer`'a aktarılabilir.

## İnceleme (Claude, 2026-10-27)
- Sınır kırpma, ölçek/ofset hesapları (i64), yerinde (src == dst) işleme (ters satır sırası), geniş görüntü yedek yolu,
  scanline/blur ve SIMD↔skaler bit-aynılık testleri incelendi. `make check` 137/137, ASan/UBSan uyarısı yok. Değişiklik gerekmedi.
