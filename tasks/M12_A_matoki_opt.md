DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M12_A — Matoki derleyici iyileştirmeleri (M10_E P1–P5)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turlardaki incelemelerde bozuk/düşmanca girdiler bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle. Ölçmeden "hızlandı" yazma.
Matoki'ye dokunduğun için `docs/MATOKI.md` de okunur. Ölçüm altyapısı hazır: `tools/host/run_matoki_vs_wat.sh`
(M10_E; elle yazılmış en iyi WAT ile karşılaştırma) ve `tasks/M10_E_matoki_vs_wat.md` Notlar (P1–P5 önerileri, ayrıntılı gerekçe).

## Hedef
Matoki JIT çıktısını elle yazılmış WAT'a yaklaştırmak. Öncelik sırası (her birini ayrı commit, önce/sonra ölçümüyle):
1. **P2** `sin/cos`: π indirgemesi + işaret biti XOR ile çağrı başına **tek polinom**; çok terimli toplamlarda yazmaç
   basıncını düşüren sıralama (plasma). Doğruluk: mevcut testlerdeki tolerans içinde kalmalı.
2. **P1** Quad-tekdüze (quad-uniform) analiz: `dFdx/dFdy` ve yalnız bunlara + uniform'lara bağlı değerler (mip seviyesi,
   `w>>level`, taban ofseti) skaler hesaplanır; 4-lane skalerleştirilmiş kaydırma (`emit_varying_shift`) bu yolda kalkar.
3. **P3** 0..255 maskeli texel kanallarında `f32x4.convert_i32x4_s`; bilinear'da 1/255 ölçeğini lerp sonrasına taşıma.
4. **P4** Basit, discard'sız ve kısa (<64 komut) çekirdeklerde 4× döngü açma + `v128.load32_splat`.
5. **P5** Sabit alfa eşiği karşılaştırmasını tamsayı alanında yapmak.
Zaman yetmezse P2 → P1 → P3 sırasıyla bitir, kalanı Notlar'a "ertelendi" yaz.

## Dosyalar (YALNIZCA bunlar)
- `src/matoki/*.c/.h` (lower, codegen, opt, ir ...)
- `tests/test_matoki_*.c` (yeni testler: tek polinom trig doğruluğu, quad-tekdüze sınıflandırma, convert_s eşdeğerliği)
- `tools/host/bench_matoki_vs_wat.mjs`, `tools/host/run_matoki_vs_wat.sh` (yalnız gerekiyorsa)
- `tasks/M12_A_matoki_opt.md`

## Kabul
- [ ] `make check` hatasız/uyarısız (ASan/UBSan)
- [ ] `run_matoki_vs_wat.sh`: 4 shader'da doğruluk testleri GEÇTİ; vcolor/sprite/plasma/lit3d Matoki/WAT verimi önce-sonra tablosu
  (aynı makinede, nöbetleşe medyan) Notlar'a; plasma ve lit3d'de ölçülebilir artış
- [ ] Tüm gömülü shader'lar (`shaders/*.mtk`, pbr3d ve dungeon3d dahil) hâlâ derleniyor; C referansıyla karşılaştıran
  mevcut testler geçiyor
- [ ] Optimize edilmiş IR/wasm'ın `mdis` iç döngü komut sayıları önce/sonra Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/app/*`, `src/raster/*`, `shaders/*`, `web/*` değişmez. Dil semantiği değişmez (yalnız kod üretimi).
Öneriler Notlar'a.

## Notlar
