# T1 — Matoki diferansiyel fuzz testi (ir_eval kahin)

Durum: TODO

## Amaç
Rastgele ama geçerli Matoki programları (pixel + compute) üreten tohumlu üreteç; her programı `ir_eval` (kahin) ile wasm
JIT (SIMD + relaxed build) karşısında çalıştır, çıktıları ulp toleransıyla karşılaştır. Fark bulunca en küçük programa
indirgeyip `tests/fuzz_cases/` altına regresyon olarak kaydet. Bulunan derleyici hatalarını düzelt (src/matoki/*).
`make fuzz N=…` hedefi; make check içinde küçük sabit tohumlu sürüm (< 10 s).
Fikir kaynağı: SamurayBalgami/Matoki ir_eval kahin yaklaşımı (izinli).

## Dosyalar
`tests/test_matoki_fuzz.c` (yeni), `tests/fuzz_cases/*` (yeni), `src/matoki/*` (yalnız hata düzeltme), `Makefile`

## Kabul kriterleri
- [ ] 100 000 program fuzz çalıştırması; bulunan/düzeltilen hata sayısı Notlar'da
- [ ] make check temiz
