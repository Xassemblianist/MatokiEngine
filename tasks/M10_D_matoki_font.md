DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M10_D — Kendi yazı tipimiz "Matoki Sans" + TTF metin çizimi

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turdaki incelemelerde bozuk/düşmanca girdi hataları bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle.

## Hedef
Dış font dosyası kullanmadan kendi yazı tipimizi üretmek: bir Python betiği (yalnız stdlib) glifleri **kod olarak** tanımlar
(çizgi + ikinci derece Bezier konturları, ASCII 32..126 + Türkçe ğĞıİşŞçÇöÖüÜ + birkaç sembol), tutarlı metriklerle
(units_per_em 1000, x-height, cap-height, ascent/descent) geçerli bir **TTF** (glyf/loca/cmap4/hmtx/kern) yazar.
Sonra mevcut `src/font` (TTF ayrıştırıcı + rasterleyici + atlas) ile bu fontu kullanan metin çizimi: atlas → alfa harmanlı
`Framebuffer` çizimi, renk, kerning, satır kaydırma (word wrap), hizalama (sol/orta/sağ), gölge/kontur seçeneği.
Font dosyası `shaders/` gibi `tools/embed.py` ile gömülebilir hale getirilmez (Makefile yasak) — bunun yerine betik ayrıca
`src/font/matoki_sans_data.c` (u8 dizi) üretir.

## Dosyalar (YALNIZCA bunlar)
- `tools/make_matoki_sans.py` (glif tanımları + TTF yazıcı), üretilen `assets/fonts/MatokiSans.ttf` ve `src/font/matoki_sans_data.c/.h`
- `src/font/text.h/.c` — `TextStyle`, `text_draw(Framebuffer*, const FontAtlas*, i32 x, i32 y, const char* utf8, const TextStyle*)`,
  `text_measure`, `text_wrap`
- `tests/test_text.c`
- `tasks/M10_D_matoki_font.md`

## Kabul
- [ ] `make check` hatasız/uyarısız
- [ ] Üretilen TTF mevcut `ttf_parse`'tan geçer; tüm tanımlı kod noktaları glif bulur; konturlar kapalı ve sargı yönü tutarlı
      (dış CW, delik CCW — rasterleyicide "O", "a", "e", "ğ" delikleri boş)
- [ ] 12/16/24/48 px'te atlas kurulur; "Ağaç kökü İğdır'da şöyle" ölçümü ve çizimi tutarlı (taban çizgisi hizası testle)
- [ ] Word wrap: verilen genişliği hiçbir satır aşmaz, kelime bölünmez (tek kelime genişlikten uzunsa karakter bazlı)
- [ ] Çizim kırpma: negatif x/y, ekran dışı, sıfır boyutlu fb → sınır dışı yazım yok (ASan)
- [ ] Görsel kanıt: 3 boyutta örnek metni PPM'e yazan test (build/test_out/) — Notlar'a nasıl göründüğünü yaz
- [ ] Benchmark: 16 px'te 1000 karakterlik paragraf çizim süresi

## Yasaklar
`docs/*`, `Makefile`, `src/font/ttf.*`, `raster.*`, `atlas.*` (hata bulursan Notlar'a yaz, düzeltme)
`src/ui/*`, `src/app/*` değişmez.
