# MATOKI Mimari v2 — Temel (motorun altındaki katman)

Durum: TASLAK (Faz 0 kanıtlamaları sürüyor) · 2026-10-29 · Sahibi: yönetici (Claude)
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

- Durum bölgesinde işaretçi yok; sim adımı belirlenimci (aynı girdi → aynı bit). Paralellik sonucu sıraya bağlamaz.
- JS'te motor mantığı yok (yükleyici seçimi ve RHI yürütme dışında).
- Bellek yalnız arena/halka/havuz; malloc yok.
- Her yeni yol eskisiyle (CPU↔GPU, özelleştirilmiş↔genel) karşılaştırma testiyle gelir.

## 7. Faz 0 — Kanıtlanacak hipotezler

| # | Hipotez | Ölçüt (geçme eşiği) | Görev |
|---|---|---|---|
| H1 | Çalışma anı özelleştirilmiş ECS döngüsü genel C döngüsünü belirgin geçer | 100k varlık, 3 sistem: ≥ 2× | F0_B |
| H2 | Aynı Matoki `compute` çekirdeği wasm ve WGSL'de aynı sonucu verir | bit/ulp toleransı, 3 çekirdek | F0_A |
| H3 | Kare grafiği ek yükü küçük, boru hattı verimi artırır | 200 düğüm < 0.1 ms; boru hattı ≥ 1.3× | F0_C |
| H4 | Durum anlık görüntüsü rollback'e yeter | 16 MB < 2 ms; 8 kare geri sarma bütçede | F0_D + M16_A |
| H5 | GPU'da yaşayan sahne CPU'yu boşaltır | 100k örnek, CPU gönderimi < 0.5 ms | Faz 1 (M15 sonrası) |
| H6 | Hedef özellikler gerçekte var | tarayıcı/özellik tablosu, kaynaklı | F0_E |

Hipotez çökerse ilgili ilke bu belgede revize edilir; karar DECISIONS'a yazılır.

## 8. Fazlar

- **Faz 0:** kanıtlamalar (F0_A..F), M15/M16 temelleri.
- **Faz 1 (L0–L1):** bellek bölgeleri, iş çalan zamanlayıcı v2, render worker, RHI, Matoki çekirdek türleri + önbellek.
- **Faz 2 (L2–L3):** ECS v2 (durum bölgesinde), GPU sahnesi, kare grafiği + boru hattı.
- **Faz 3 (L4):** render v2 (visibility buffer, meshlet, compute raster, sanal gölge/doku, GI), rollback ağ, akış.

## 9. Ajan ataması (bkz. hafıza: agent-model-routing)

Argon high → derleyici/RHI/grafik gibi karmaşık uygulama; Flash high → ölçüm, test, dar kapsamlı işler;
Claude Sonnet/Opus (agy) → inceleme, mimari eleştiri, doğrulama. Yönetici → kararlar, görev tanımı, kabul.
