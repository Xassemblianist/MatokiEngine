# MATOKI Mimari v2 — Temel (motorun altındaki katman)

Durum: TASLAK r2 (F0_F eleştirisi işlendi, bkz. `ARCH_V2_REVIEW.md`) · 2026-10-29 · Sahibi: yönetici
İlgili kararlar: D-105 (paralel ön uç), D-108 (WebGPU), D-109 (platform tavanı). Vizyon: "webin Unreal'i".

Bu belge oyun motorunu değil, **motorun üzerine kurulacağı temeli** tanımlar. Her ilke ölçülebilir bir hipoteze
bağlıdır; Faz 0 kanıtlamaları hipotezi doğrulamadan ilgili katman kodlanmaz (ARCHITECTURE §0.3).

## 0. İlkeler

1. **Tavanı hedefle, taban yalnız çalışsın** (D-109). En yeni web özellikleri varsayılan; tespit + yedek.
2. **Tek hesap dili.** CPU (wasm SIMD) ve GPU (WGSL) kodu aynı ara dilden (Matoki IR) üretilir. Algoritma bir kez yazılır.
3. **Motor kendini sahneye göre derler.** Sıcak döngüler genel amaçlı değil; çalışma anında veri düzenine, sabitlere ve
   materyale özelleştirilmiş çekirdeklerdir. Derleyici bir araç değil, çalışma zamanının parçasıdır.
4. **Tek zamanlayıcı, iki işlemci.** CPU işleri ve GPU geçişleri aynı kare grafiğinde; iş, onu daha hızlı yapan tarafa gider.
5. **Durum tek bellek, zaman bir eksen.** Simülasyon durumu işaretçisiz, ofsetli, kopyalanabilir tek bölge → anlık görüntü,
   rollback, zamanda geri sarma, durum kaybetmeden canlı yenileme.
6. **Veri GPU'da yaşar.** Sahne GPU'da kalıcı; CPU yalnız değişiklik (delta) yollar. Çizim kararları compute'ta.
7. **Ölçmeden yok.** Her katmanın bütçesi ve testi var; CPU ile GPU çıktısı birbirinin referansıdır.

## 1. Katmanlar

```
L4  Alt sistemler: render v2, fizik, animasyon, ses, ağ (rollback), varlık akışı, UI, editör
L3  Kare grafiği: CPU iş düğümleri + GPU geçiş/compute düğümleri, kaynak ömrü, kare boru hattı
L2  Veri: ECS arketip depoları (durum bölgesinde), GPU sahnesi, sanal varlık sayfaları
L1  Hesap alt katmanı: Matoki IR → wasm (CPU) | WGSL (GPU); çekirdek önbelleği; çalışma anı özelleştirme
L0  Çalışma zamanı: bellek modeli, iş parçacıkları, iş çalan zamanlayıcı, RHI komut akışı, yetenek raporu
    Platform: wasm32 (+relaxed SIMD, threads), WebGPU, Worker/OffscreenCanvas, AudioWorklet, WebTransport/RTC
```
Kural: bir katman yalnız altındakini çağırır. L4 doğrudan RHI'ye ya da job sistemine gitmez; L3 düğümü olarak gider.

## 2. L0 — Çalışma zamanı

**Bellek modeli** (tek paylaşımlı wasm belleği, wasm32 → 4 GB tavan; memory64 Faz 0'da yalnız ölçülür):

| Bölge | Ömür | İçerik | Not |
|---|---|---|---|
| Kalıcı | uygulama | kod önbelleği, tablolar | arena |
| Durum (State) | snapshot'lanabilir | ECS depoları, fizik, oyun durumu | **işaretçi yok**, yalnız ofset/tutamaç; tek blok kopyayla anlık görüntü |
| Kare halkası ×3 | kare | kare N (render), N+1 (sim), N−1 (GPU'da) geçicileri | kare boru hattı için 3 halka |
| Akış önbelleği | LRU | varlık sayfaları (geometri, doku) | sanal sayfa tabloları |
| GPU yükleme halkası | kare | GPU'ya gidecek delta'lar | RHI `write_buffer` kaynağı |

**İş parçacıkları:** ana iş parçacığı yalnız DOM/girdi köprüsü; **render worker** OffscreenCanvas + WebGPU cihazının sahibi;
N **işçi** (iş çalan deque'ler, öncelik sınıfları: kare-kritik / arka plan / akış); **AudioWorklet** gerçek zamanlı ses.
Hepsi tek SharedArrayBuffer üzerinde kilitsiz kuyruklarla konuşur.

**RHI** (D-108, M15_B): tutamaçlı kaynaklar + kare başına düz ikili komut akışı; JS yalnız yürütür.

## 3. L1 — Hesap alt katmanı (Matoki)

Matoki IR tek ara dildir. Çekirdek türleri:

| Tür | Anlam | CPU hedefi | GPU hedefi |
|---|---|---|---|
| `pixel` | quad başına SPMD (bugünkü shader) | wasm SIMD, 4 lane | WGSL fragment |
| `vertex` | köşe başına | wasm SIMD | WGSL vertex |
| `compute` | N eleman üzerinde SPMD, buffer okuma/yazma, atomik | wasm SIMD + iş parçacıkları | WGSL compute |
| `system` | ECS arketip döngüsü (bileşen SoA'ları) | wasm SIMD, arketip başına özel | (gerekirse) compute |

**Çalışma anı özelleştirme:** çekirdek önbellek anahtarı = (kaynak özeti, veri düzeni, sabit uniform'lar, hedef).
Arketip/mesh düzeni/materyal değişince yeniden derlenir (ms mertebesi); sonuç tüm işçilere çoğaltılır.

## 4. L2 — Veri

- **ECS:** arketip başına SoA, durum bölgesinde; varlık tutamacı = (indeks, nesil). Sistemler `system` çekirdeği.
- **GPU sahnesi:** örnek tablosu (dönüşüm, sınır, materyal kimliği), materyal tablosu, meshlet havuzu, doku sayfa
  tablosu GPU'da kalıcı; CPU kare başına yalnız kirli aralıkları yollar.
- **Sanal varlıklar:** geometri (meshlet kümeleri + LOD), doku ve gölge sayfaları talep üzerine akar.

## 5. L3 — Kare grafiği

Düğüm türleri: `cpu` (iş), `gpu_pass` (render), `gpu_compute`, `transfer`. Her düğüm okuduğu/yazdığı kaynakları
bildirir. Grafik derleyicisi: topolojik sıra, CPU→GPU bağımlılıkları, bariyerler, geçici kaynak ömrü ve takma adlama,
**kare boru hattı** (sim N+1 ∥ render kaydı N ∥ GPU N−1). Aynı mantıksal iş (ör. görünürlük, parçacık, skinning) hem
`cpu` hem `gpu_compute` gerçeklemesine sahip olabilir; zamanlayıcı ölçülen maliyete göre seçer.

## 6. Değişmezler

**Belirlenimcilik sınıfları (D-112):**

| Sınıf | Kapsam | İzinli | Yasak | Test |
|---|---|---|---|---|
| Sim-kritik | ECS sim, fizik, oyun mantığı, hash'e giren her şey | non-relaxed wasm SIMD, sabit bloklu paralellik | relaxed SIMD, GPU, FMA daralması, NaN, float atomik, duvar saati | 1T=2T=8T hash; tarayıcılar arası hash |
| Yalnız-görsel | render, skin, gölge, parçacık görseli, ses | relaxed SIMD, GPU, dinamik CPU↔GPU seçimi | sim durumuna geri yazmak | tolerans + görüntü hash'i |

Sim, render'a yalnız **extract** düğümüyle (kare N'in salt-okunur kopyası) veri verir; render sim durumunu okumaz.

- Durum bölgesinde işaretçi yok; sim adımı belirlenimci (aynı girdi → aynı bit). Paralellik sonucu sıraya bağlamaz.
- JS'te motor mantığı yok (yükleyici seçimi ve RHI yürütme dışında).
- Bellek yalnız arena/halka/havuz; malloc yok.
- Her yeni yol eskisiyle (CPU↔GPU, özelleştirilmiş↔genel) karşılaştırma testiyle gelir.

## 7. Faz 0 — Kanıtlanacak hipotezler

| # | Hipotez | Ölçüt (geçme eşiği) | Görev |
|---|---|---|---|
| H1 | Çalışma anı özelleştirilmiş ECS döngüsü en iyi elle yazılmış C/SIMD döngüsünü geçer | 3 kol (genel / elle SIMD / özel): özel ≥ 1.3× elle SIMD; derleme geri ödeme süresi; p95 derleme+çoğaltma | F0_B |
| H2 | Aynı Matoki `compute` çekirdeği wasm ve WGSL'de aynı sonucu verir | bit/ulp toleransı, 3 çekirdek | F0_A |
| H3 | Kare grafiği ek yükü küçük, boru hattı verimi artırır | 200 düğüm < 0.1 ms; boru hattı ≥ 1.3× | F0_C |
| H4 | Rollback bütçesi | gerçek fizik+ECS ile 8 kare geri sar + yeniden simüle ≤ 12 ms p99 | F0_D + M16_A + P1 |
| H5 | GPU'da yaşayan sahne CPU'yu boşaltır | 100k örnek, CPU gönderimi < 0.5 ms | Faz 1 (M15 sonrası) |
| H6 | Hedef özellikler gerçekte var | karar kuralı: hedef kitlenin ≥ %80'inde varsa tavan, yoksa tespitli seçenek; Safari/Firefox asgari küme | F0_E |
| H7 | Belirlenimcilik | fizik+ECS 10 000 tick: {1T,2T,8T} × {Chrome, Firefox} aynı hash dizisi | F0_G |
| H8 | Fizik v2 | 2 000 dinamik cisim @ 120 Hz, 8T adım ≤ 1.8 ms, 1T = 8T hash | P1 |
| H9 | JIT yaşam döngüsü | 200 özelleştirme/oturum: tablo/arena sızıntısı 0, kare p99 sıçraması < 4 ms | F0_B ek |
| H10 | Yedek kademe | SAB yok → tek iş parçacıklı derleme tam çalışır, CI'da koşar | K1 |
| H11 | Gecikme | girdi → foton, boru hattı derinliği 1/2/3 ölçülür | F0_C ek |
| H12 | Bütçe | orta segment mobilde 60 fps hedef senaryo + bellek tavanı tablosu | Faz 1 |

Hipotez çökerse ilgili ilke bu belgede revize edilir; karar DECISIONS'a yazılır.

## 8. Fazlar

Sıra (r2): belirlenimcilik ve durum yerleşimi, her sistemin veri düzenini belirlediği için GPU render v2'den **önce** gelir.
- **Faz 0:** kanıtlamalar (F0_A..G), M15/M16 temelleri, fizik taşıma (P1_0).
- **Faz 1 (L0 + sim çekirdeği):** tick zamanlayıcı, girdi olay halkası, ofset tabanlı durum bölgesi, iş zamanlayıcı v2
  (deque + öncelik + servis katmanı: IO/ağ/derleme kare grafiği dışında), tek iş parçacıklı yedek derleme, fizik v2 (P1_A..E).
- **Faz 2 (L1–L3):** IR v2 kararı + Matoki çekirdek türleri, JIT yaşam döngüsü, ECS v2 (durum bölgesinde), GPU sahnesi,
  kare grafiği + extract düğümü + boru hattı.
- **Faz 3 (L4, daraltılmış):** render v2 yalnız GPU-sürümlü kırpma + dolaylı çizim + kümelenmiş ileri aydınlatma ile başlar;
  meshlet/sanal gölge/GI ancak bunlar ölçülünce. Rollback ağı, varlık hattı (cook/paket/sürüm), akış.

## 8b. Açık kararlar (r2)
- **IR v2 kapsamı:** bugünkü IR quad-piksel IR'ıdır (bellek/döngü/atomik/struct yok). F0_A sonucu: kısıtlı şablon mu, IR v2 mi.
- **Oyun mantığı dili:** C + wasm AOT mi, Matoki genişletme mi (Ö-11).
- **Ağ:** rollback + sunucu-yetkili anlık görüntü iki mod; WebRTC/WebTransport worker'da (F0_E).
- **Headless tarayıcı CI:** `tools/host/webgpu_check.mjs` temelli; test araçlarında JS istisnası ve altın görüntü/hash yöntemi.
- **Varlık hattı:** cook aracı, paket formatı, doku sıkıştırma (KTX2/Basis benzeri).

## 9. Ajan ataması (bkz. hafıza: agent-model-routing)

Argon high → derleyici/RHI/grafik gibi karmaşık uygulama; Flash high → ölçüm, test, dar kapsamlı işler;
Claude Sonnet/Opus (agy) → inceleme, mimari eleştiri, doğrulama. Yönetici → kararlar, görev tanımı, kabul.
