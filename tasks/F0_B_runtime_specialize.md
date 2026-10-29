# F0_B — Çalışma anı özelleştirme kanıtı: arketip başına JIT ECS döngüsü (H1)

Durum: TODO

## Amaç
ARCH_V2 ilke 3 / H1. Mevcut `src/scene` ECS'inde 100k varlık, 3 sistem (hareket: pos += vel*dt; yerçekimi+sürtünme;
sınır kutusunda yansıma). İki yol: (a) genel C döngüsü (mevcut sorgu API'si), (b) çalışma anında arketip düzenine
ve sabitlere (dt, g, kutu) özel **wasm modülü üretilir** (mevcut `masm`/`mbin` + `jit` altyapısı veya Matoki IR) ve
JIT ile yüklenir: SoA ofsetleri sabit, dalsız, SIMD. Host'ta (wasm üretimi doğrulama) + tarayıcı ölçüm sayfası.

## Dosyalar
- `src/spec/*` (yeni: özelleştirici), `tests/test_spec.c` (yeni), `web/spec_bench.html` + `web/spec_bench.js` (yeni)
- `Makefile` (yalnız yeni kaynaklar), `src/app/main.c` (yalnız bench dışa aktarımı)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] Özelleştirilmiş ve genel yol aynı sonucu verir (bit-bit, test)
- [ ] Tarayıcı ölçümü sayfası hazır; host'ta ölçebildiğin kadarı + derleme süresi (ms) Notlar'a. ≥ 2× hedefi tutmazsa
      nedenini analiz et (bu da değerli sonuç)
