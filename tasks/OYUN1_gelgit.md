# OYUN1 — "Gelgit": fırtınalı okyanusta yelkenli (motorun ilk gerçek oyunu)

Durum: IN_PROGRESS (agy gemini-4-argon-high)

> **Öne çekildi (2026-10-29):** Yalnız G1_A (GPU okyanus, main'de) gerekli. V1 (atmosfer/bulut), G1_B (GPU sahnesi), P1_A/B (fizik)
> paralel çalışıyor ya da bekliyor: onları BEKLEME, mevcut `src/phys2` (ya da main'de hangisi varsa) ve G1_A'nın GPU okyanus/gök
> yolunu kullan. Öncelik: **grafik kalitesi ve oynanabilir dikey dilim**. Sıra: prototip (tekne+rüzgâr+dalga) → görsel cila
> (GPU yolunda, 1080p) → görevler. Limit kısıtlı: önce oynanabilir + güzel bir sürümü main'e al (merge.sh), sonra iyileştir.
Model: gemini-4-argon-high

## Neden Argon, neden bu oyun
Argon'un bu projede kanıtlanmış güçlü yanı: **fiziksel olarak doğru görsel simülasyonu tek seferde kurmak**.
M17_A'da Tessendorf FFT + JONSWAP + Jacobian köpük + kaldırma kuvvetini yazdı; ölçülen Hs 2.17 m, teori 2.14 m.
Genel olarak da güçlü olduğu alanlar: uzamsal akıl yürütme, shader/GPU kodu, prosedürel içerik, matematik ağırlıklı
sistemler. Zayıf yanı: kendi işini abartarak raporlamak ve var olmayanı "var" saymak.
Bu görev ikisini de hesaba katar. Oyunun kalbi Argon'un en iyi yaptığı şey: dalga, rüzgâr, su, ışık. Her iddia ise
**ölçümle ve ekran görüntüsüyle** kanıtlanır.

## Oyunun özü (tek cümle)
Tek kişilik bir yelkenliyle, **dalgayı ve rüzgârı okuyarak** yaklaşan fırtınadan önce adalar arasında yük taşıdığın,
her yolculuğun gerçek su fiziğiyle yeniden şekillendiği kısa ama derin bir denizcilik oyunu.

Fantezi: "Denizi gerçekten anlıyorum". Oyuncu dalga setlerini sayar, rüzgâr kaymasını bulut ve su yüzeyinden görür,
dalganın sırtından sörf yaparak hız kazanır, yanlış açıda yakalanınca alabora olur. Ekranda hiçbir şey sahte değil:
gördüğün dalga, tekneyi iten dalgadır.

## Tasarım sütunları (her karar bunlara göre verilir)
1. **Okunabilir gerçeklik:** Fizik gerçekçi ama oyuncu nedenini görebilmeli. Her kuvvetin görsel ya da işitsel bir
   ipucu var: rüzgâr şiddeti → yelken titremesi, su yüzeyindeki "kedi pençesi" dalgacıkları, ip gerginliğinin sesi.
2. **Beceri tavanı yüksek, giriş eşiği düşük:** İlk 30 saniyede herkes ilerler; usta oyuncu dalga sörfü ve sert
   rüzgârla aynı rotayı yarı sürede alır.
3. **Her yolculuk farklı, her yolculuk adil:** Hava tohumlu ve belirlenimci. Aynı tohum = aynı deniz, böylece
   meydan okumalar ve hayalet yarışlar adil olur.
4. **Sessiz güzellik:** Arayüz minimal, dünya konuşur. Gün batımında 10 saniye hiçbir şey yapmadan bakmak bile
   ödüllendirici olmalı.

## Çekirdek döngüler
- **30 saniye:** Rüzgârı oku → yelken ve dümen ayarla → dalgaya göre çizgi seç → hız/denge geri bildirimi.
- **5 dakika:** Bir adadan yük al → fırtına cephesi yaklaşırken rota seç (kısa ama açık deniz / uzun ama kıyı
  korumalı) → teslim et → ödül.
- **1 saat:** Tekne yükseltmeleri (yelken kesimi, salma, gövde), yeni adalar, artan fırtına şiddeti, günlük meydan
  okuma tohumu.

## Mekanikler (kesin tanımlar)
**Yelken fiziği (sim-kritik, CPU, belirlenimci):**
- Görünür rüzgâr = gerçek rüzgâr − tekne hızı.
- Yelken kaldırma/sürükleme: hücum açısına göre basit polar (C_L, C_D tablosu, tamsayı tick'te sabit adım).
- Yelken açısı (iskota), dümen, salma: yanal direnç. Böylece rüzgâra ~45°'ye kadar orsa seyri ve "tacking" mümkün.
- Ölü bölge: rüzgâra tam karşı yelken boşa düşer, sesli ve görsel uyarı.
- Bayılma ve alabora: yatma momenti > doğrulma momenti (salma + mürettebat ağırlığı). Oyuncu "ağırlık at" tuşuyla
  karşı koyar. Alabora 3 saniyelik kurtarma mini etkileşimidir, oyun bitmez ama yük kaybolabilir.

**Dalga etkileşimi:**
- Kaldırma: gövde üzerinde 8–12 örnek noktası, su yüksekliği CPU FFT'den (sim-kritik). Sürüklenme suya batmış hacimle.
- Dalga sörfü: tekne dalga ön yüzünde ve yeterince hızlıysa yerçekimi bileşeni ileri iter. Hız göstergesinde ayrı
  renk, ses değişir. Doğru zamanlama = %30–60 hız artışı.
- Dalga gövdeye çarpınca hız kaybı ve sprey (sprey yalnız görsel, GPU).

**Hava ve fırtına:**
- Tohumlu hava cephesi: rüzgâr yönü/şiddeti zamanla değişir (cephe hattı haritada hareket eder).
- JONSWAP parametreleri rüzgâra bağlı: Hs fırtınada 6 m+.
- Rüzgâr hamleleri (gust) yüzeyde görülür, koyu "kedi pençesi" lekesi tekneye ulaşmadan 2–3 saniye önce fark edilir.
- Görüş: yağmur, sis, gece. Fener ve deniz feneri ışıkları kümelenmiş aydınlatmayla.

**Navigasyon:**
- Pusula, rüzgâr gülü (görünür ve gerçek rüzgâr), log ile hız. Harita yok: deniz fenerleri, kara siluetleri ve
  yıldızlar ile yön bulunur. İsteğe bağlı kolay modda kâğıt harita.

## İçerik (dikey dilim)
- 1 takımada: 4 ada (V2 arazisi varsa kullan; yoksa basit prosedürel kıyı), 2 deniz feneri, 1 liman.
- 6 görev: öğretici (rüzgâr, tacking, sörf, alabora kurtarma) + 2 gerçek teslimat (biri fırtınada).
- 1 "Günün denizi" modu: tohumlu parkur, kendi en iyi koşunla hayalet yarışı (replay = girdi akışı, src/sim).
- 3 tekne yükseltmesi.

## Görsel yön
- Gerçekçi ama stilize ışık: altın saat ve gün batımı öne çıkar (V1 atmosferi), fırtınada kurşuni-yeşil palet.
- Köpük (Jacobian), sprey parçacıkları, yelken kumaşı (XPBD; görsel sınıf, CPU'da rüzgâra göre şişer), ıslak güverte
  parlaması, ufukta yağmur perdeleri.
- Kamera: teknenin arka çeyreğinde, dalga ritmine göre yumuşatılmış, sörfte hafif FOV artışı ve sarsıntı;
  serbest bakış (fare/sağ çubuk).
- HUD minimal: yalnız rüzgâr gülü, hız ve yük durumu. Gerisi dünyada (bayrak, flama, su).

## Ses
- Rüzgâr: şiddete ve görünür rüzgâr açısına göre filtrelenmiş gürültü.
- Teknenin sesleri: gövdede su (hıza göre), ip gerginliği gıcırtısı, yelken çırpması (boşa düştüğünde).
- Dalga kırılmaları: 3D konumlu (HRTF). Fırtınada gök gürültüsü, şimşekle senkron.
- Sentez src/audio içinde (örnek dosyası gerekmez).

## Kontroller
- Klavye: A/D dümen, W/S iskota, Shift ağırlık at, Space yük/etkileşim.
- Gamepad: sol çubuk dümen, tetikler iskota, A etkileşim.
- Dokunmatik: sol yarı dümen kaydırma, sağ yarı iskota kaydırma.
- Input buffer 100 ms; dümen ivmeli (ani değil, gerçek dümen hissi).

## Teknik gereksinimler
- **Belirlenimcilik (D-112):** Tekne fiziği, hava, rüzgâr ve görev durumu sim-kritik: tamsayı tick, 1T=8T aynı hash.
  Köpük, sprey, yelken kumaşı, bulut, ses görsel sınıf.
- **Tekrar oynatma:** Replay aynı girdiyle aynı hash'i vermeli (hayalet yarış buna dayanır).
- **Bütçe (1080p, RTX 2060):**
  - GPU kare süresi < 10 ms.
  - CPU sim < 2 ms/tick (8T).
  - CPU yolunda da (WebGPU yok) 720p oynanabilir.
- Mevcut modülleri kullan, kopyalama: `src/gfx3d/ocean`, `src/phys` (kaldırma, rijit gövde),
  `src/sim` (tick, replay), `src/gpu`, `src/audio`, `src/ui`.
  - Eksik genel altyapı (menü yığını, girdi eşleme) gerekiyorsa küçük ve genel tut: `src/game/`.

## Çalışma sırası (bu sırayla, her adımda make check)
1. **Tasarım belgesi (kod yazmadan):** `docs/games/gelgit.md`. Yukarıdakini kendi kararlarınla netleştir: sayısal
   ayarlar (C_L/C_D tablosu, kütle, yelken alanı), ilerleme eğrisi, 6 görevin akışı, riskler.
2. **Prototip:** Yalnız tekne + rüzgâr + dalga. Tacking ve sörf hissi doğru olana kadar başka şeye geçme.
   - Ölç: orsa açısı, sörf hız artışı.
   - Ayar sabitlerini HUD'dan (debug) canlı değiştirilebilir yap.
3. **Hava ve fırtına**, ses.
4. **Görevler**, adalar, menü, kazanma/kaybetme, "Günün denizi" + hayalet.
5. **Cila:** kamera, juice, görsel efektler, performans.
6. **Doğrulama** (aşağıda), Notlar, merge.

## Kabul kriterleri (hepsi gözlenmiş olmalı; uydurma = görev başarısız)
- [ ] `docs/games/gelgit.md` tasarım belgesi (sayısal ayarlarla)
- [ ] Yeni sahne "Gelgit" ana menüden açılıyor; öğretici + 2 teslimat görevi baştan sona oynanabiliyor
- [ ] Ölçümler (gerçek): orsa açısı (°), sörf hız artışı (%), alabora eşiği (°), 1080p GPU ms, sim ms/tick (1T/8T),
      `uptime` yükü
- [ ] Replay: aynı girdi kaydı 3 kez oynatılınca aynı hash; 1T ve 8T'de aynı hash (testi `tests/` altında)
- [ ] Headless Chrome + Vulkan 1080p ekran görüntüleri `build/gelgit_*.png`: sakin deniz gün batımı, fırtına,
      sörf anı, alabora, liman. Her birine gerçekten bak ve Notlar'a ne gördüğünü yaz.
- [ ] WebGPU kapalıyken CPU yolunda 720p oynanabiliyor
- [ ] `make check` + `make detcheck` + `make gpucheck` temiz
- [ ] Notlar: neyin "eğlenceli" olduğunu, neyin olmadığını dürüstçe yaz (oynanış testi gözlemleri), ertelenenler

## Dosyalar
- Yeni: `src/game/*`, `src/app/scene_gelgit.*`, `src/gfx3d/sail.*` (yelken kumaşı görseli), `shaders/*`
  (gerekirse yeni), `tests/test_gelgit.c`, `docs/games/gelgit.md`
- Değişebilir: `src/app/main.c` (sahne kaydı), `src/gfx3d/ocean.*` (yalnız rüzgâra bağlı parametre API'si),
  `src/audio/*` (yalnız yeni sentez sesleri), `web/*` (yalnız girdi/gamepad/dokunmatik)
