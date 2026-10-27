DURUM: DONE
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
- [x] `make check` hatasız, uyarısız (ASan/UBSan)
- [x] Üretilen TTF'te: cmap eşlemeleri, advance/lsb, bileşik glif dönüşümü, kern değerleri birebir doğru
- [x] Rasterleyici: eksen hizalı kare tam pikselde 255/0, yarım piksel kaydırmada kenar 127/128 (±1);
      delikli konturda iç 0; alan toplamı analitik alanla ±%0.5; üçgen kenarı monoton
- [x] Sistemde `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` varsa (yoksa testi atla, geç): tüm cmap
      glifleri 8..64 px rasterleşir, çökme yok; "Ağış" ölçümü pozitif ve tutarlı. Sistem fontu repoya EKLENMEZ.
- [x] Fuzz: üretilen TTF'e 50.000 rastgele bayt mutasyonu, ASan altında çökme/sınır dışı okuma yok
- [x] Benchmark: 16/32/64 px'te 95 ASCII glif rasterleme süresi (glif/s) Notlar'a

## Yasaklar
- `docs/*`, `Makefile`, başka modüller, `src/ui/*`, `src/app/*` değişmez. Öneriler bu dosyanın Notlar'ına.

## Notlar

### Yapılanlar
- **`src/font/ttf.h/.c`**: Sıfırdan big-endian TrueType (`.ttf`) ayrıştırıcı yazıldı. Tüm tablo ve glif okumaları `u64` taşma korumalı sınır denetiminden (`check_range`) geçer. `head`, `hhea`, `hmtx` (`numberOfHMetrics < numGlyphs` kuyruk `lsb` dizisi dahil), `maxp`, `loca` (kısa `u16*2` ve uzun `u32`), `cmap` (biçim 4 `idDelta` + `idRangeOffset/glyphIdArray` ve biçim 12 grup araması), `kern` (biçim 0 yatay çift araması) ve `glyf` (basit + `WE_HAVE_A_SCALE`, `WE_HAVE_AN_X_AND_Y_SCALE`, `WE_HAVE_A_TWO_BY_TWO` dönüşümlü ve nokta eşlemeli özyinelemeli bileşik glifler) desteklendi.
- **`src/font/raster.h/.c`**: TrueType konturlarındaki ardışık `off-curve` noktaları orta nokta kuralıyla açan, De Casteljau alt-bölümlemesiyle ikinci derece Bezier eğrilerini düzleştiren (`FONT_DEFAULT_FLATNESS = 0.05f` px) ve her yönlü kenar parçasını tarama satırı `[j, j+1]` ile piksel sütunlarına `[i, i+1]` kırparak tek geçişli fark dizisine (`diff`) yamuk alanı biriktiren kenar yumuşatmalı (signed-area AA) rasterleyici uygulandı. Önek toplamı üzerinden nonzero sargı kuralı (`|sum|`) ve alt-piksel `x/y` kaydırması desteklendi.
- **`src/font/atlas.h/.c`**: Verilen piksel yüksekliği için `.notdef` + ASCII (`32..126`) + Latin-1 (`160..255`) + Türkçe (`ğĞıİşŞçÇöÖüÜ`) gliflerini 1 px dolgulu raf (shelf) algoritmasıyla tek bir `u8` alfa atlasına paketleyen `FontAtlas`, $O(1)$ tablo araması (`cp < 384`), RFC 3629 uyumlu UTF-8 çözücü (`font_atlas_utf8_next`) ve kerning destekli çok satırlı `font_measure` yazıldı.
- **`tests/assets/make_font.py` & `tests/test_font.c`**: Yalnız Python standart kütüphanesiyle üçgen, eksen hizalı kare, delikli "O", Bezier eğrisi, ölçekli ve 2×2 matrisli bileşik glifler, `cmap` biçim 4+12 ve `kern` biçim 0 içeren `tests/assets/test_font.ttf` (888 bayt) üretildi. Tüm kabul kriterleri, `DejaVuSans.ttf` üzerinde 8..64 px tüm cmap gliflerinin rasterlenmesi ve tam boyutlu ASan yığın tamponunda 50.000 rastgele bayt/kesme mutasyonu fuzz testi doğrulandı.

### Benchmark (Intel Core i7-10750H @ 2.60GHz, `clang -O1 -fsanitize=address,undefined`, `DejaVuSans.ttf` 95 ASCII glif `32..126`, 5 ısınma + 15 koşum medyanı)
| Boyut | 95 Glif Medyan Süre | Hız (glif/s) |
|---:|---:|---:|
| **16 px** | 0.364 ms | **260.870 glif/s** |
| **32 px** | 0.515 ms | **184.309 glif/s** |
| **64 px** | 1.003 ms | **94.696 glif/s** |

### Ertelenenler ve Öneriler
- **Ertelenenler**: OpenType `GPOS` tabanlı kerning, CFF (`OTTO`) üçüncü derece Bezier konturları, hinting bytecode yorumlayıcısı.
- **Öneriler**: `FontAtlas` çıktısını `Texture` (`RGBA8` veya tek kanallı alfa shader varyantı) olarak `Batch2D` ve `src/ui/` ile bağlayarak ölçeklenebilir UI metin çizimi sağlamak.

## İnceleme (Claude, 2026-10-27)
- Bileşik glif açılımında derinlik (16) ve bileşen (256) sınırları tek başına yetmiyordu: kendine başvuran çok bileşenli
  bir glif 256^16 çağrıya şişip motoru kilitleyebilirdi → glif başına toplam düğüm bütçesi `TTF_MAX_GLYPH_NODES` (1024).
  Test: `ttf_review_self_referencing_composite` (6 → 6,6 glifi < 50 ms'de reddedilir).
- Rasterleyici fark dizisi sınırları, atlas paketleme taşma denetimleri ve cmap/kern/loca sınır denetimleri incelendi: sorun yok.
