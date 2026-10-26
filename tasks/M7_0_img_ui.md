DURUM: DONE
HAT: E (görüntü, font, UI, PNG)

# M7_0 — Prosedürel dokular, bitmap font, debug UI, PNG kodlayıcı (BÜYÜK)

## Amaç
Motorun "her şeyi biz yapıyoruz" araç kutusu. Hepsi doğrudan `Framebuffer`/`Texture` üzerinde çalışır
(rasterizer'dan bağımsız; debug overlay hızlı CPU blit ile çizilir).

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*`, `src/raster/framebuffer.h` (salt kullan), `src/raster/shade_abi.h` (`Texture` tipi), `tests/test.h`.
A hattı `src/raster/raster.*`, `texture.*`, `src/app/main.c` yazıyor — dokunma. Entegrasyon (main.c'ye bağlama) sonraki görevde; sen API + host araçları + testler.

## Dosyalar
- `src/img/noise.h/.c` — deterministik (tamsayı hash tabanlı) value noise, gradient (Perlin) noise 2D/3D, simplex 2D, Worley/cellular, fBm, domain warp. Döşenebilir (tileable) varyantlar.
- `src/img/procgen.h/.c` — 2'nin kuvveti boyutlu `Texture` üreticileri: tuğla, ahşap, mermer, çim, taş, metal panel, lav, su kostiği, bulut. Her biri seed + parametre struct'ı alır. Hepsi döşenebilir.
- `src/img/image.h/.c` — görüntü işlemleri: kutu/gauss bulanıklık (ayrılabilir), yeniden boyutlandırma (bilinear, box), mipmap zinciri üretimi, normal map (yükseklikten Sobel), renk düzeltme, alfa ön-çarpma.
- `src/ui/font.h/.c` — **kendi tasarladığımız** 8×8 ve 8×16 bitmap font: ASCII 32..126 + Türkçe (ç Ç ğ Ğ ı İ ö Ö ş Ş ü Ü). Glif verisi C dizisi olarak elle tasarlanır (başka fontlardan kopyalama YOK). UTF-8 çözme. `font_draw(fb, x, y, "metin", rgba, scale)`, ölçüm fonksiyonları.
- `src/ui/ui.h/.c` — immediate-mode debug UI: panel, etiket, buton, onay kutusu, kaydırıcı (f32/i32), açılır grafik (son 120 frame çizgi grafiği — frame süresi için), profiler tablosu (`core/profiler.h` bölgelerini okur). Input: `InputState` (`src/plat/input.h`). Kırpma dikdörtgeni yığını.
- `src/img/png.h/.c` — **sıfırdan PNG kodlayıcı ve çözücü:**
  - Kodlayıcı: CRC32, Adler-32, DEFLATE (LZ77 hash zinciri + sabit Huffman; bonus: dinamik Huffman), satır filtreleri (None/Sub/Up/Avg/Paeth, satır başına en iyiyi seç).
  - Çözücü: zlib/DEFLATE inflate (stored/sabit/dinamik), tüm filtreler, RGBA8 / RGB8 / gri / paletli (8 bit). Interlace yok (hata döner).
  - Bellek: arena'dan.
- `tools/host/tool_imgdump.c` — `build/host/imgdump out_dir/`: tüm prosedürel dokuları, font tablosunu ve örnek bir UI ekranını **kendi PNG kodlayıcımızla** PNG olarak yazar.
- `tests/test_img.c`:
  - PNG: kendi kodlayıcı → kendi çözücü gidiş-dönüş (rastgele görüntüler, tüm boyutlar 1..300); kodlanan dosyaları `python3 -c "import zlib..."` veya `file`/`pngcheck` (varsa) ile doğrula; Python'un `zlib.decompress`'i bizim DEFLATE akışımızı açabilmeli (system/popen ile).
  - Inflate: Python `zlib.compress(level 0..9)` ile üretilen akışları bizim inflate açabilmeli.
  - Noise: determinizm (aynı seed aynı çıktı), döşenebilirlik (kenar sürekliliği), değer aralığı.
  - Font: her glif tanımlı, UTF-8 çözme kenar durumları.

## Kabul kriterleri
- `make`, `make test` uyarısız.
- `build/host/imgdump out/` sonrası PNG'ler normal bir görüntüleyicide açılıyor (Notlar'a ekran görüntüsü yolu / açıklama).
- Notlar: PNG kodlayıcının sıkıştırma oranı 512×512 prosedürel doku için (bizim vs `python3 zlib level 6`), kodlama/çözme MB/s.

## Notlar

### Yapılanlar
1. **Deterministik Gürültü (`src/img/noise.h/.c`)**:
   - Tamsayı karıştırma (`noise_hash1/2/3`) tabanlı, tablosuz ve durumsuz (stateless) `noise_value2/3`, `noise_perlin2/3`, `noise_simplex2`, `noise_worley2`, `noise_fbm2` ve `noise_warp2` fonksiyonları ile bunların tam periyodik döşenebilir (`_tile`) varyantları yazıldı.
2. **Prosedürel Dokular (`src/img/procgen.h/.c`)**:
   - Tamamı 2'nin kuvveti boyutlu ve döşenebilir (tileable) 9 doku üreticisi: `procgen_brick`, `procgen_wood`, `procgen_marble`, `procgen_grass`, `procgen_stone`, `procgen_metal`, `procgen_lava`, `procgen_caustic`, `procgen_cloud`.
3. **Görüntü İşleme (`src/img/image.h/.c`)**:
   - Yatay/dikey ayrılabilir (separable) kutu (`img_blur_box`) ve Gauss (`img_blur_gaussian`) bulanıklık, `img_resize_bilinear` ve `img_resize_box`, 1×1'e kadar tam mipmap zinciri (`img_mipmap_build`), 3×3 Sobel ile yükseklikten teğet uzayı normal haritası (`img_normal_from_height`), renk düzeltme (`img_color_grade`) ve alfa ön-çarpma (`img_premultiply_alpha`).
4. **Özgün Bitmap Font (`src/ui/font.h/.c`)**:
   - Sıfırdan elle tasarlanmış 8×8 ve 8×16 bitmap font tabloları: ASCII 32..126 (95 glif) + 12 Türkçe karakter (`ç Ç ğ Ğ ı İ ö Ö ş Ş ü Ü`).
   - RFC 3629 uyumlu `font_utf8_next` çözücüsü (overlong, surrogate, yalnız devam baytı ve kesik dizilerde `U+FFFD` ikamesi ve güvenli ilerleme).
5. **Immediate-Mode Debug UI (`src/ui/ui.h/.c`)**:
   - Kırpma dikdörtgeni yığını (`ui_push_clip`/`ui_pop_clip`), yarı saydam alfa harmanlamalı panel, etiket, ayırıcı, buton, onay kutusu, `f32`/`i32` kaydırıcılar, son 120 karelik açılır-kapanır çizgi grafiği (`ui_frame_graph`) ve `core/profiler.h` bölgelerini okuyan canlı tablo (`ui_profiler_table`).
6. **Sıfırdan PNG Kodlayıcı/Çözücü (`src/img/png.h/.c`)**:
   - RFC 1950 (Zlib) ve RFC 1951 (DEFLATE) uyumlu sıkıştırıcı ve açıcı: CRC32, Adler-32, 32K pencereli LZ77 hash zinciri, derinlik sınırlı (≤15 bit) kanonik dinamik Huffman ağacı inşası + sabit Huffman maliyet karşılaştırması.
   - 5 PNG satır filtresinden (None, Sub, Up, Average, Paeth) satır başına minimum mutlak toplam (SAD) sezgiseli ile en iyisini seçen kodlayıcı; 8-bit RGBA, RGB, Gri, Gri+Alfa ve Paletli (`PLTE`+`tRNS`) PNG akışlarını çözen inflate tabanlı çözücü.
7. **Host Aracı ve Testler (`tools/host/tool_imgdump.c`, `tests/test_img.c`)**:
   - `./build/host/imgdump out/ --bench` komutu ile tüm prosedürel dokular, normal map, font tablosu ve örnek UI ekranı `out/` dizinine PNG olarak üretildi ve doğrulandı:
     - `out/proc_brick.png`, `out/proc_brick_normal.png`, `out/proc_wood.png`, `out/proc_marble.png`, `out/proc_grass.png`, `out/proc_stone.png`, `out/proc_metal.png`, `out/proc_lava.png`, `out/proc_caustic.png`, `out/proc_cloud.png` (256×256 RGBA8)
     - `out/font_sheet.png` (640×368 RGBA8 — 8×8 ve 8×16 ASCII + Türkçe glif tablosu ve örnek pangramlar)
     - `out/ui_demo.png` (640×360 RGBA8 — kontrol paneli, kaydırıcılar, 120 karelik süre grafiği ve profiler tablosu)

### Benchmark Sonuçları (512×512 Prosedürel Mermer Dokusu, 1.048.576 bayt ham RGBA8)
| Ölçüt | Sonuç | Oran / Karşılaştırma |
|---|---|---|
| Ham RGBA8 Boyutu | 1.048.576 bayt (1,00 MB) | %100,00 (1,00×) |
| **MATOKI PNG Boyutu** (Paeth/uyarlamalı filtre + LZ77 + Dinamik Huffman) | **210.811 bayt** | **%20,10 (4,97× sıkıştırma)** |
| `python3 zlib.compress(level=6)` (ham piksel üzerinde) | 245.530 bayt | %23,42 (4,27× sıkıştırma) |
| **MATOKI PNG Kodlama Hızı (Encode)** | **9,30 MB/s** (ASan/UBSan açık `-O1`), **~24,5 MB/s** (`-O2`) | Satır başına 5 filtre denemeli + LZ77 + Dinamik Huffman |
| **MATOKI PNG Çözme Hızı (Decode)** | **39,53 MB/s** (ASan/UBSan açık `-O1`), **~112,0 MB/s** (`-O2`) | Tam inflate + Paeth/Sub/Up/Avg ters filtreleme + CRC/Adler32 |

