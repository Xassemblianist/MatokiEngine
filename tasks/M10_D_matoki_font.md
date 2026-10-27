DURUM: DONE
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
- [x] `make check` hatasız/uyarısız
- [x] Üretilen TTF mevcut `ttf_parse`'tan geçer; tüm tanımlı kod noktaları glif bulur; konturlar kapalı ve sargı yönü tutarlı
      (dış CW, delik CCW — rasterleyicide "O", "a", "e", "ğ" delikleri boş)
- [x] 12/16/24/48 px'te atlas kurulur; "Ağaç kökü İğdır'da şöyle" ölçümü ve çizimi tutarlı (taban çizgisi hizası testle)
- [x] Word wrap: verilen genişliği hiçbir satır aşmaz, kelime bölünmez (tek kelime genişlikten uzunsa karakter bazlı)
- [x] Çizim kırpma: negatif x/y, ekran dışı, sıfır boyutlu fb → sınır dışı yazım yok (ASan)
- [x] Görsel kanıt: 3 boyutta örnek metni PPM'e yazan test (build/test_out/) — Notlar'a nasıl göründüğünü yaz
- [x] Benchmark: 16 px'te 1000 karakterlik paragraf çizim süresi

## Yasaklar
`docs/*`, `Makefile`, `src/font/ttf.*`, `raster.*`, `atlas.*` (hata bulursan Notlar'a yaz, düzeltme)
`src/ui/*`, `src/app/*` değişmez.

## Notlar

### Yapılanlar
- **`tools/make_matoki_sans.py`, `assets/fonts/MatokiSans.ttf` (10.512 bayt, 129 glif, 91 kerning çifti) ve `src/font/matoki_sans_data.c/.h`**:
  - Yalnız Python standart kütüphanesi kullanılarak `.notdef`, tüm basılabilir ASCII (`32..126`), 12 Türkçe karakter (`ğ Ğ ı İ ş Ş ç Ç ö Ö ü Ü`), şapkalı Türkçe ünlüler (`â î û Â Î Û`) ve tipografik semboller (`NBSP © « ® ° ± · » × ÷ – — • … ₺`) ikinci derece Bezier eğrileri ve doğru parçaları olarak kodlandı.
  - Metrikler: `units_per_em = 1000`, `ascent = 800`, `descent = -200`, `line_gap = 100`, `cap_height = 660`, `x_height = 500`, `ascender = 680`, `descender = -180`.
  - Her konturun sargı yönü analitik işaretli alan hesabıyla denetlenerek dış konturlar CW (`+y` yukarı Kartezyen uzayda alan `< 0`), iç delikler CCW (alan `> 0`) olarak yönlendirildi; betik çalışırken tüm delikli gliflerin (`O, D, P, Q, R, B, A, a, b, d, e, g, o, p, q, 0, 4, 6, 8, 9, ğ, Ö, ö`) delik merkezlerinde net sargı sayısının tam `0` olduğunu doğrular.
- **`src/font/text.h/.c`**:
  - `TextStyle`, `text_measure`, `text_wrap`, `text_wrap_ex` ve `text_draw` sıfırdan, dinamik bellek (`malloc`/`Arena`) ayırmadan akışlı (`WrapIter`) olarak uygulandı.
  - Alfa harmanlama (`div255_round` kaydırma özdeşliğiyle bölmesiz hızlı yol), `kern` biçim 0 kerning, sol/orta/sağ hizalama (`TEXT_ALIGN_LEFT/CENTER/RIGHT`), gölge (`shadow_color`, `shadow_dx`, `shadow_dy`) ve Öklid diski kontur (`outline_color`, `outline_px <= TEXT_MAX_OUTLINE_PX`) desteklendi.
  - Güvenlik: Tüm ekran koordinatları `i64` ile hesaplanıp `[0..fb->w) x [0..fb->h)` aralığına kırpılır (`I32_MIN`/`I32_MAX` taşması önlenir); tek karakterin bile `max_width`'i aştığı durumda en az 1 UTF-8 kod noktası ilerlenerek sonsuz döngü engellenir; girdi uzunluğu `TEXT_MAX_INPUT_BYTES` (1 MB) ve kontur yarıçapı `TEXT_MAX_OUTLINE_PX` (8 px) ile sınırlandırılmıştır.
- **`tests/test_text.c`**:
  - Diskteki `MatokiSans.ttf` ile gömülü `g_matoki_sans_ttf` dizisinin bayt-bayt eşdeğerliği, tüm kod noktalarının `cmap` biçim 4 ve 12'de bulunması, kontur sargı yönleri ve `"O", "a", "e", "ğ"` dahil 23 delikli glifin 48 px raster çıktısında dört yönden çevrili boş delik (`alpha == 0`) içerdiği doğrulandı.
  - 12, 16, 24 ve 48 px boyutlarında `FontAtlas` kurulumu, `"Ağaç kökü İğdır'da şöyle"` ölçüm/çizim uyumu ve taban çizgisi (`baseline`) hizası (alçalmasız harflerin tam `baseline_y` satırında bittiği, alçalan `"ğ, ç, ş, y"` harflerinin `baseline_y` altına indiği) doğrulandı.
  - Kelime bölmeden satır kaydırma (uzun tek kelimede UTF-8 karakter sınırından bölme dahil), sol/orta/sağ hizalama ve ASan yığın tamponlarında negatif/ekran dışı/sıfır boyutlu `Framebuffer` kırpma testleri eklendi.

### Görsel Kanıt (`build/test_out/matoki_sans_3sizes.ppm`, 860×380)
- **48 px başlık**: Koyu lacivert (`#121620`) arka plan üzerinde sıcak krem rengi (`#FFF0B4`), 1 px koyu mavi kontur ve `(3, 3)` px yarı saydam siyah gölgeyle `"Ağaç kökü İğdır'da şöyle"` metni; Bezier yayları pürüzsüz, `'a'`, `'ö'`, `'e'`, `'d'`, `'ğ'` iç delikleri tam açık, `'ğ'` breve aksanı ve `'ç'`/`'ş'` çengelleri net.
- **24 px alt başlık**: Açık mavi (`#8CDCFF`) gölgeli iki satır (`"24px: Ağaç kökü İğdır'da şöyle «0123456789» ©®°±×÷"` ve `"Pijamalı hasta yağız şoföre çabucak güvendi."`); rakamlar, semboller ve Türkçe karakterler aynı taban çizgisinde dengeli.
- **16 px gövde ve hizalama sütunları**: Tüm büyük/küçük ASCII + Türkçe alfabe satırı ve altında 250 px genişlikte kelime sarmalanmış 3 sütun (sol hizalı yeşil, orta hizalı turuncu, sağ hizalı eflatun) temiz okunurlukta.

### Benchmark (Intel Core i7-10750H @ 2.60GHz, `clang -O1 -fsanitize=address,undefined`, 5 ısınma + 15 koşum medyanı)
| Senaryo | Medyan Süre | Hız |
|---|---:|---:|
| **16 px, 1000 karakterlik Türkçe paragraf (`max_width = 600 px`, kerning + word wrap + AA blend)** | **0.454 ms** | **2.205 karakter/ms (~2.21 M karakter/s)** |

### Mevcut `src/font/atlas.c` Hakkında Gözlem (Yasaklar gereği dokunulmadı)
- `src/font/atlas.c` içindeki `collect_codepoints` yalnızca `.notdef`, ASCII (`32..126`), Latin-1 (`160..255`) ve 6 ek Türkçe harfi (`Ğ ğ İ ı Ş ş`) atlasa paketlemektedir; bu nedenle `U+015F` üstündeki genel Unicode sembolleri (`₺` `U+20BA`, `—` `U+2014`, `…` `U+2026`) `TtfFont` (`ttf_glyph_index`) içinde tanımlı olsa da `FontAtlas` paketleme listesine girmez. İleride `collect_codepoints` dizisine sık kullanılan genel noktalama/para birimi kod noktalarının eklenmesi önerilir.
