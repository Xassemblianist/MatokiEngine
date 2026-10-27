DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M9_D — JPEG çözücü (baseline + progressive, sıfırdan)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, libc YOK (`src/` altında).
Örnek arayüz: `src/img/png.h` (`png_decode` → arena'dan RGBA u32, `FB_RGBA` düzeni `raster/framebuffer.h`).

## Dosyalar (YALNIZCA bunlar)
- `src/img/jpeg.h/.c` — `u32* jpeg_decode(const u8* data, usize len, u32* w, u32* h, Arena* arena);`
  SOI/APPn/DQT/SOF0/SOF1/SOF2/DHT/DRI/SOS/EOI; Huffman (hızlı arama tablosu), restart işaretleri,
  bayt doldurma (FF00), 8 bit; 1 (gri) ve 3 bileşen (YCbCr); örnekleme 4:4:4, 4:2:2, 4:2:0 (+ gerekirse genel h/v);
  tamsayı IDCT (AAN veya LLM, sabit nokta); kroma yükseltme (en az çift-doğrusal "fancy" yükseltme seçeneği);
  progressive (SOF2: DC/AC ilk + iyileştirme taramaları). CMYK/Adobe, 12 bit, aritmetik kodlama: hata döner.
  EXIF yönü okunur ve `jpeg_orientation()` ile bildirilir (döndürme yapılmaz).
  Hata durumunda NULL + `jpeg_last_error()` dizesi. Tüm okumalar sınır denetimli.
- `src/img/jpeg_wasm.c` (isteğe bağlı) — IDCT / renk dönüşümü için wasm SIMD sürümü; skaler sürümle bit-aynı olmalı
- `tests/test_jpeg.c`, `tests/assets/make_jpeg.py` — test görüntülerini Python standart kütüphanesi ile
  PPM/PGM olarak üretir ve `ffmpeg` ile (sistemde var) çeşitli JPEG'lere kodlar: gri, 4:4:4, 4:2:2, 4:2:0,
  restart aralıklı, progressive, tek blok olmayan boyutlar (ör. 33×17, 1×1, 257×129). Üretilen küçük .jpg'ler
  ve referans `.ppm` çıktıları (ffmpeg ile çözülmüş) repoya girer; toplam < 300 KB.
- `tasks/M9_D_jpeg.md`

## Kabul kriterleri
- [x] `make check` hatasız, uyarısız (ASan/UBSan)
- [x] Her test JPEG'i için ffmpeg'in çözdüğü referansa karşı: ortalama mutlak fark ≤ 1.0, en büyük fark ≤ 8
      (IDCT/yükseltme farkları için); gri görüntüde ≤ 1 ve ≤ 3
- [x] Bozuk dosyalar (kesik, yanlış işaret, aşırı boyut, sıfır Huffman) NULL + hata döner
- [x] Fuzz: her test dosyasına toplam 50.000 rastgele bayt mutasyonu, ASan altında çökme yok
- [x] Benchmark (host): 1024×1024 4:2:0 baseline ve progressive çözme süresi (MP/s) Notlar'a.
      `jpeg_wasm.c` yazıldıysa skaler/SIMD bit-aynılığı testte doğrulanır.

## Yasaklar
- `docs/*`, `Makefile`, `src/asset/*` (glTF bağlantısını entegratör yapacak), başka modüller değişmez.

## Notlar

### Yapılanlar ve Tasarım Seçimleri
- **`src/img/jpeg.h` ve `src/img/jpeg.c`**:
  - Sıfırdan, dış bağımlılıksız ve `src/` altında hiçbir libc başlığı kullanmadan Baseline (`SOF0`), Extended Sequential (`SOF1`, 8-bit) ve Progressive (`SOF2`) JPEG çözücü yazıldı.
  - Tüm geçici çalışma tamponları (MCU/katsayı tamponları ve satır yükseltme çalışma alanı) `arena_temp_begin` / `arena_temp_end` ile geri sarılır; kalıcı olarak yalnızca son `w * h * sizeof(u32)` `FB_RGBA` piksel dizisi `arena`'da kalır.
  - **Huffman çözücü**: 9-bit hızlı arama tablosu (`JPEG_HUFF_FAST_BITS = 9`) + uzun kodlar (10..16 bit) için sınır denetimli yedek yol. Eksik, sıfır sembollü veya aşırı abone (oversubscribed) Huffman tabloları reddedilir.
  - **Bit akışı ve Restart**: `0xFF 0x00` bayt doldurma (byte stuffing), `DRI` ve `RST0..RST7` (`0xFFD0..0xFFD7`) sıfırlama aralıkları hem baseline hem progressive (DC/AC ilk ve iyileştirme) taramalarında desteklenir.
  - **Progressive (`SOF2`)**: Tam 16-bit (`i16`) katsayı ızgarası üzerinde çoklu tarama; DC ilk (`Ah == 0`), DC iyileştirme (`Ah > 0`), AC ilk (`Ah == 0`, EOBn koşuları dahil) ve AC iyileştirme (`Ah > 0`, ZRL ve sıfır/sıfır-olmayan katsayı bit düzeltmeleri dahil) eksiksiz gerçeklendi.
  - **IDCT**: Loeffler-Ligtenberg-Moschytz (LLM) sabit noktalı tamsayı 8×8 2B ayrılabilir IDCT (12-bit/13-bit sabit nokta ölçekli).
  - **Kroma yükseltme ve renk dönüşümü**: Varsayılan `jpeg_decode()` çift-doğrusal ("fancy" 3/4 - 1/4 merkezli üçgen filtre) kroma yükseltme kullanır; `jpeg_decode_ex(..., fancy_upsample)` ile en yakın komşu (kutu) yükseltme de seçilebilir. 4:4:4, 4:2:2, 4:2:0 hızlı yollarının yanı sıra genel `h/v` (`1..4`) alt örnekleme oranları ve tekil boyutlar (`33×17`, `1×1`, `257×129`) desteklenir. ITU-R BT.601 tam aralıklı (JFIF) sabit noktalı YCbCr → RGBA dönüşümü kullanıldı.
  - **EXIF yönü**: `APP1` (`Exif\0\0`) içindeki TIFF başlığı hem little-endian (`II`) hem big-endian (`MM`) olarak ayrıştırılır; `0x0112` (Orientation) etiketi `1..8` aralığında doğrulanıp `jpeg_orientation(data, len)` ile döndürülür (ve son `jpeg_decode` çağrısında güncellenir).
  - **Desteklenmeyen / bozuk girdiler**: 12-bit/16-bit (`precision != 8`), aritmetik kodlamalı (`SOF9`/`SOF10`), kayıpsız (`SOF3`), CMYK/4 bileşenli, `APP14` Adobe (`transform != 0/1`) veya RGB (`C1='R', C2='G', C3='B'`) başlıkları, `16384×16384` üstü boyutlar, kesik akışlar ve bozuk işaretçiler `NULL` döner ve `jpeg_last_error()` ile Türkçe hata açıklaması verir.
- **`src/img/jpeg_wasm.c`**:
  - WebAssembly 128-bit SIMD (`<wasm_simd128.h>`, `i32x4`) ile 8×8 IDCT (`jpeg_idct_block`: 4 sütun paralel 1B LLM kelebek + iki adet 4×4 `v128_t` matris devriği + 4 satır paralel 1B LLM kelebek) ve 4-piksel paralel YCbCr → `FB_RGBA` dönüşümü (`jpeg_ycbcr_to_rgba_row`) gerçeklendi.
  - Skaler kod ile yuvarlama ve kaydırma sırası birebir aynı tutularak **%100 bit-aynı (bit-exact)** çıktı sağlandı ve `test_jpeg.c` içinde hem rastgele 256 blok/satır hem de çözülen tüm görüntüler üzerinde wasm32 ikilisi koşturularak doğrulandı.
- **Test varlıkları (`tests/assets/make_jpeg.py`)**:
  - Python standart kütüphanesi ile deterministik test görüntüleri üretildi; `ffmpeg` (8.0.1) ile JPEG kodlaması ve referans `.ppm` çözümleri üretildi. (`ffmpeg`'in `mjpeg` kodlayıcısı `-progressive` bayrağı olmadığı için `prog_420.jpg` standart JFIF `SOF2` 6-taramalı progressive akışı olarak `make_jpeg.py` içinde üretilip referansı yine `ffmpeg -i prog_420.jpg` ile çözüldü.)
  - `tests/assets/` altındaki 9 adet `.jpg` + 9 adet `*_ref.ppm` dosyasının toplam boyutu **154.1 KB** (< 300 KB sınırı).

### Referans Karşılaştırma Sonuçları (`ffmpeg` `.ppm` referansına karşı)

| Test Görüntüsü | Boyut | JPEG Boyutu | Ortalama Mutlak Fark (`mean_diff`) | En Büyük Fark (`max_diff`) | Kabul Sınırı |
|---|---:|---:|---:|---:|---|
| `gray.jpg` (1 bileşen, gri) | 48×48 | 894 B | **0.0260** | **1** | ort ≤ 1.0, maks ≤ 3 |
| `color_444.jpg` (4:4:4) | 48×48 | 886 B | **0.0428** | **3** | ort ≤ 1.0, maks ≤ 8 |
| `color_422.jpg` (4:2:2) | 48×48 | 836 B | **0.7745** | **4** | ort ≤ 1.0, maks ≤ 8 |
| `color_420.jpg` (4:2:0) | 48×48 | 796 B | **0.9171** | **4** | ort ≤ 1.0, maks ≤ 8 |
| `restart_420.jpg` (`DRI=4`, 4:2:0) | 48×48 | 1,201 B | **0.9171** | **4** | ort ≤ 1.0, maks ≤ 8 |
| `prog_420.jpg` (`SOF2`, 4:2:0) | 48×48 | 1,359 B | **0.9171** | **4** | ort ≤ 1.0, maks ≤ 8 |
| `odd_33x17.jpg` (4:2:0, tekil) | 33×17 | 468 B | **0.4266** | **3** | ort ≤ 1.0, maks ≤ 8 |
| `odd_1x1.jpg` (4:4:4, 1×1) | 1×1 | 205 B | **0.0000** | **0** | ort ≤ 1.0, maks ≤ 8 |
| `odd_257x129.jpg` (4:2:0, tekil) | 257×129 | 8,376 B | **0.2023** | **3** | ort ≤ 1.0, maks ≤ 8 |

### Benchmark Sonuçları (1024×1024 4:2:0 — 1.048576 MP)
- **Donanım / Ortam**: Intel Core i7-10750H @ 2.60GHz (Linux x86_64), Clang 19.1.7, Node.js v22.21.1 (Wasm32 V8).
- **Girdi**: 1024×1024 YCbCr 4:2:0 sentetik sahne (Baseline `SOF0`: 225,683 bayt; Progressive `SOF2` 6-tarama: 279,135 bayt).
- **Ölçüm Yöntemi**: 3 ısınma turu + 15 ölçüm turunun medyanı (`CLOCK_MONOTONIC` / `performance.now()`).

| Yapılandırma | 1024×1024 4:2:0 Baseline | 1024×1024 4:2:0 Progressive | Not |
|---|---:|---:|---|
| **Host Native Release (`-O2`)** | **11.51 ms (91.1 MP/s)** | **13.79 ms (76.1 MP/s)** | Saf C11 skaler (`src/img/jpeg.c`) |
| **Host Test (`-O1 -fsanitize=address,undefined`)** | **33.19 ms (31.6 MP/s)** | **41.90 ms (25.0 MP/s)** | `make test` ASan + UBSan açıkken |
| **Wasm32 Skaler (`-O2`, `jpeg.c`)** | **18.21 ms (57.6 MP/s)** | **21.37 ms (49.1 MP/s)** | Node V8 üzerinde skaler wasm32 |
| **Wasm32 SIMD (`-O2 -msimd128`, `jpeg_wasm.c`)** | **12.17 ms (86.2 MP/s)** | **15.89 ms (66.0 MP/s)** | Skaler wasm32'ye göre **1.50×** (baseline) / **1.35×** (progressive) hızlı, **bit-aynı** |

