# P1_E — Fizik benchmark + regresyon paketi

Durum: TODO

## Amaç
`tools/host/bench_phys.c` + `make benchphys`: yığın kulesi (20 kat), 10k kutu, kumaş 64×64, araç, ragdoll 50, karakter.
Her senaryo: ms/adım (1T, 8T), enerji kayması, kararlılık (kule 10 s ayakta), belirlenimcilik hash'i. Hash'ler
`tests/phys_hashes.txt`'de; make check küçük sürümü çalıştırır, değişim → test hatası.

## Dosyalar
`tools/host/bench_phys.c` (yeni), `tests/test_phys_regress.c` (yeni), `tests/phys_hashes.txt` (yeni), `Makefile`

## Kabul kriterleri
- [ ] Tablo Notlar'da
- [ ] make check temiz
