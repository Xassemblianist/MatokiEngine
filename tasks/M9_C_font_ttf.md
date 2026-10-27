DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M9_C — TrueType yazı tipi ayrıştırıcı ve rasterleyici (sıfırdan)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, libc YOK (`src/` altında).
Tipler `core/types.h`, matematik `core/math.h`, bellek `core/arena.h` (malloc yok). Mevcut bitmap font `src/ui/` altında; ona dokunma.

## Dosyalar (YALNIZCA bunlar)
- `src/font/ttf.h/.c` — TTF (glyf tabanlı, big-endian) ayrıştırıcı: tablo dizini + checksum yok sayılabilir,
  `head`, `hhea`, `hmtx`, `maxp`, `loca` (kısa/uzun), `glyf` (basit + bileşik glif, dönüşümlü), `cmap`
  (biçim 4 ve 12), `kern` (biçim 0). Tüm ofsetler sınır denetimli: bozuk dosya asla sınır dışı okumaz, hata döner.
- `src/font/raster.h/.c` — glif konturu → ikinci derece Bezier düzleştirme → kapsama (alan) tabanlı kenar
  yumuşatmalı rasterleyici (signed-area accumulation, stb_truetype'ın yeni rasterleyicisi gibi; kod kopyalama YOK),
  nonzero sargı kuralı, alt piksel x konumu.
- `src/font/atlas.h/.c` — FontAtlas: verilen piksel boyutu için glifleri (ASCII + Latin-1 + Türkçe ğĞıİşŞçÇöÖüÜ)
  tek bir u8 alfa atlasına raf (shelf) paketleme; glif ölçüleri (advance, bearing, kutu, uv); `font_measure`,
  UTF-8 dize çözme, kerning uygulanır.
- `tests/test_font.c`, `tests/assets/make_font.py` (yalnız Python standart kütüphanesi ile birkaç glifli
  minimal bir TTF üretir: kare, üçgen, delikli "O" benzeri kontur, bileşik glif, cmap4+12, kern) + üretilen `.ttf`
- `tasks/M9_C_font_ttf.md`

## Kabul kriterleri
- [ ] `make check` hatasız, uyarısız (ASan/UBSan)
- [ ] Üretilen TTF'te: cmap eşlemeleri, advance/lsb, bileşik glif dönüşümü, kern değerleri birebir doğru
- [ ] Rasterleyici: eksen hizalı kare tam pikselde 255/0, yarım piksel kaydırmada kenar 127/128 (±1);
      delikli konturda iç 0; alan toplamı analitik alanla ±%0.5; üçgen kenarı monoton
- [ ] Sistemde `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` varsa (yoksa testi atla, geç): tüm cmap
      glifleri 8..64 px rasterleşir, çökme yok; "Ağış" ölçümü pozitif ve tutarlı. Sistem fontu repoya EKLENMEZ.
- [ ] Fuzz: üretilen TTF'e 50.000 rastgele bayt mutasyonu, ASan altında çökme/sınır dışı okuma yok
- [ ] Benchmark: 16/32/64 px'te 95 ASCII glif rasterleme süresi (glif/s) Notlar'a

## Yasaklar
- `docs/*`, `Makefile`, başka modüller, `src/ui/*`, `src/app/*` değişmez. Öneriler bu dosyanın Notlar'ına.
