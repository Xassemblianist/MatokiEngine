# P1_B — Rijit çözücü paralel ölçekleme

Durum: TODO

## Amaç
phys rijit çözücüsü 8T'de yalnız ~1.1× hızlanıyor (P1_0 notları). Hedef ≥ 4× (10k kutu yığını):
kısıt grafiği renklendirme + renk başına paralel iş, ada başına paralel geniş/dar faz, SIMD temas çözücü (4 temas/lane),
iş zamanlayıcı yükü düşük (renk başına tek `job_parallel_for`, küçük renkler seri). **Belirlenimcilik korunur**: 1T=2T=8T
aynı hash (D-112), relaxed SIMD yok, FMA kasılması yok.

## Dosyalar
`src/phys/*`, `src/thread/*` (yalnız gerekiyorsa), `tests/test_phys*.c`, `tools/host/bench_phys.c`

## Kabul kriterleri
- [ ] 10k kutu: 1T vs 8T ms tablosu (makine yükü notlu); hedef ≥ 4×
- [ ] make detcheck: 1T/2T/8T hash aynı
- [ ] make check temiz
