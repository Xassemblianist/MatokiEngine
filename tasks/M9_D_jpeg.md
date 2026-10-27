DURUM: TODO
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
- [ ] `make check` hatasız, uyarısız (ASan/UBSan)
- [ ] Her test JPEG'i için ffmpeg'in çözdüğü referansa karşı: ortalama mutlak fark ≤ 1.0, en büyük fark ≤ 8
      (IDCT/yükseltme farkları için); gri görüntüde ≤ 1 ve ≤ 3
- [ ] Bozuk dosyalar (kesik, yanlış işaret, aşırı boyut, sıfır Huffman) NULL + hata döner
- [ ] Fuzz: her test dosyasına toplam 50.000 rastgele bayt mutasyonu, ASan altında çökme yok
- [ ] Benchmark (host): 1024×1024 4:2:0 baseline ve progressive çözme süresi (MP/s) Notlar'a.
      `jpeg_wasm.c` yazıldıysa skaler/SIMD bit-aynılığı testte doğrulanır.

## Yasaklar
- `docs/*`, `Makefile`, `src/asset/*` (glTF bağlantısını entegratör yapacak), başka modüller değişmez.
