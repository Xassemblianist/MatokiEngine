DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M10_E — Matoki çıktısı vs elle yazılmış en iyi WAT (verim ölçümü)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turdaki incelemelerde bozuk/düşmanca girdi hataları bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle.
Matoki derleyicisine dokunmadan önce `docs/MATOKI.md` okunur. **Bu görevde derleyici DEĞİŞTİRİLMEZ** — yalnız ölçülür ve raporlanır.

## Hedef
"Matoki wasm'a yüzde kaç verimle dönüşüyor?" sorusunu ölçerek cevaplamak. `shaders/sprite.mtk`, `plasma.mtk`, `vcolor.mtk`,
`lit3d.mtk` için: (a) Matoki JIT çıktısı (`build/host/matc` ile .wasm; `--ir-opt` ile IR), (b) aynı ABI'yi (`src/raster/shade_abi.h`,
`ShadeBatch`) uygulayan **elle yazılmış, mümkün olan en hızlı WAT** (`asm/shade_*.wat`). Her ikisi de aynı girdilerde bit-bit
(veya belgelenmiş ≤1 LSB) aynı çıktıyı vermeli. Node'da (`tools/host/` altındaki mevcut .mjs kalıplarını izle) 1M quad üzerinde
Mquad/s, iç döngü komut sayısı ve kod boyutu. Sonuç: her shader için **Matoki / elle WAT oranı (%)** ve farkın nereden geldiği
(`wasm2wat` / `build/host/mdis` iç döngü karşılaştırması) — derleyiciye hangi optimizasyonların eklenmesi gerektiğine dair
somut, ölçüme dayalı öneri listesi (Notlar'a, öncelik sırasıyla, beklenen kazançla).

## Dosyalar (YALNIZCA bunlar)
- `asm/shade_sprite.wat`, `asm/shade_plasma.wat`, `asm/shade_vcolor.wat`, `asm/shade_lit3d.wat`
- `tools/host/bench_matoki_vs_wat.mjs`, `tools/host/run_matoki_vs_wat.sh` (derle + doğrula + ölç)
- `tasks/M10_E_matoki_vs_wat.md`

## Kabul
- [ ] `make check` hatasız (bu görev src'ye dokunmaz)
- [ ] 4 shader için doğruluk karşılaştırması (rastgele girdi, ≥10.000 quad) geçer
- [ ] Ölçüm: ısınma + ≥15 tekrar, medyan; tablo Notlar'a; komut satırı tekrarlanabilir
- [ ] Öneri listesi: her öneri için hangi shader'da ne kadar kazanç beklendiği (ölçülen farka dayanarak)

## Yasaklar
`src/*`, `docs/*`, `Makefile`, `shaders/*` değişmez. Uydurma sayı yazılmaz.
