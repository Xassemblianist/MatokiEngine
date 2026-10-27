DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M10_B — Retro görüntü modu (eski konsol estetiği), sıfırdan

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turdaki incelemelerde bozuk/düşmanca girdi hataları bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
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
- [ ] `make check` hatasız/uyarısız (ASan/UBSan)
- [ ] Tamsayı büyütme: her kaynak pikseli tam s×s blok; hedef kaynaktan büyük/küçük/ölçeğe bölünmüyorsa ortalanır ve kenar temizlenir, sınır dışı yazım yok
- [ ] 5:5:5 + Bayer: düz gri rampada ortalama hata ≤ 1/2 LSB(5 bit); dither kapalıyken çıktı bit-bit tahmin edilebilir (tablo testi)
- [ ] Palet: 16 ve 256 renkli paletlerde LUT sonucu, kaba kuvvet en yakın renkle 32³ örnekte birebir aynı
- [ ] `retro_wasm.c` yazıldıysa skalerle bit-aynı (rastgele 1000 kare)
- [ ] Benchmark (host + not): 320×180 → 1280×720 present süresi (ms) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/gfx3d/pipeline.*`, `src/raster/*`, `src/app/*` değişmez. Öneriler Notlar'a.
