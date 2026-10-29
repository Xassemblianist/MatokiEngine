# M16_A — Deterministik simülasyon çekirdeği + rollback (netcode temeli)

Durum: TODO

## Amaç
Vizyonun 2. sütunu: tarayıcıda gecikmesiz çok oyunculu oyun için motorun parçası olan rollback çekirdeği
(ağ katmanı sonraki iş; bu görev ağsız).
- `src/sim/sim.h/.c`: sabit adımlı simülasyon döngüsü (ör. 60 Hz), oyuncu başına girdi halkası (kare numaralı),
  tahmini girdi (son girdiyi tekrarla), gecikmeli gerçek girdi gelince **geri sar + yeniden simüle et**.
- Durum anlık görüntüsü: oyun durumu tek bir arena bölgesinde tutulur; snapshot = o bölgenin memcpy'ı (halka,
  ör. 16 kare). İşaretçi yerine ofset/indeks kullanımı belgelensin.
- Durum özeti (hash, ör. FNV-1a/xxhash benzeri, kendi yazımın) ile senkron kaybı tespiti.
- Belirlenimcilik denetimi: `phys` 3D fiziğinin ve ECS'in aynı girdilerle bit-bit aynı sonuç verdiğini testle doğrula;
  bozan yerler varsa (ör. sırasız iş paralelliği, NaN) Notlar'a yaz ve mümkünse düzelt.
- Girdi kaydı/oynatma (replay) — aynı kayıttan aynı hash dizisi.

## Dosyalar
- `src/sim/*` (yeni), `tests/test_sim.c` (yeni), `Makefile` (yalnız yeni kaynaklar gerekiyorsa)
- `src/phys/*`, `src/scene/*` (yalnız belirlenimcilik düzeltmeleri)
- `docs/DECISIONS.md` (D-110 kaydı), `docs/ARCHITECTURE.md` (modül listesi)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] Test: iki "eş" simülasyon, biri girdileri 1–8 kare gecikmeli alıyor; rollback sonrası her karede hash'ler aynı
- [ ] Test: 10 000 kare fizik + ECS replay iki kez → hash dizileri aynı
- [ ] Ölçüm: 8 kare geri sarma + yeniden simülasyon maliyeti (örnek sahne) Notlar'da
