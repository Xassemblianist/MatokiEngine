# F0_C — Kare grafiği v0: CPU + GPU düğümleri, kare boru hattı (H3)

Durum: TODO

## Amaç
ARCH_V2 §5. `src/graph/`: düğüm (cpu | gpu_pass | gpu_compute | transfer), kaynak bildirimi (okur/yazar, geçici/
kalıcı), derleme (topolojik sıra, bağımlılık sayaçları, geçici kaynak ömrü + takma adlama planı), yürütme: cpu
düğümleri mevcut job sistemi üzerinde bağımlılık sayaçlarıyla (bariyersiz, hazır olan çalışır); gpu düğümleri şimdilik
kayıt fonksiyonu çağrısı (RHI M15_B gelince bağlanacak — arayüzü buna uygun bırak). Kare boru hattı: sim N+1 ile render
kaydı N'nin örtüşmesi (3 kare halkası).

## Dosyalar
- `src/graph/*` (yeni), `tests/test_graph.c` (yeni), `Makefile` (yalnız yeni kaynaklar), `docs/ARCHITECTURE.md` (modül satırı)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] Testler: döngü tespiti, yanlış sıra yok, ömür/alias planı doğru, rastgele 1000 grafik × yürütme = seri sonuç
- [ ] Ölçüm: 200 düğümlü grafikte derleme + yürütme ek yükü (host); boru hattı kazancı (sentetik iş yüküyle)
