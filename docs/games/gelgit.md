# OYUN1 — "Gelgit": Fırtınalı Okyanusta Yelkenli Tasarım Belgesi

Durum: AKTİF TASARIM · Görev: `tasks/OYUN1_gelgit.md` · Mimari Uyum: D-108 (WebGPU RHI), D-110 (Sim/Replay), D-112 (Belirlenimcilik Sınıfları)

## 1. Oyunun Özü ve Tasarım Sütunları

**Tek cümlelik öz:** Tek kişilik ahşap bir yelkenliyle, **dalgayı ve rüzgârı okuyarak** yaklaşan fırtınadan önce adalar arasında yük taşıdığın, her yolculuğun gerçek su fiziğiyle yeniden şekillendiği kısa ama derin bir denizcilik oyunu.

### Tasarım Sütunları
1. **Okunabilir Gerçeklik:** Kuvvetlerin hiçbirisi gizli bir zamanlayıcıdan gelmez. Rüzgâr yönü direk tepesindeki kırmızı flamadan, rüzgâr gülünden ve su yüzeyinde ilerleyen koyu "kedi pençesi" (gust) lekelerinden okunur; yelken hücum açısı yanlışsa yelken bezi pırpır eder (luffing) ve ses verir; dalga sırtına oturulduğunda pruva eğimi ve hız göstergesi turkuaz-altın renge döner.
2. **Düşük Giriş Eşiği, Yüksek Beceri Tavanı:** İlk 30 saniyede `A/D` (dümen) ve `W/S` (iskota) ile herkes tekneyi yürütebilir. Usta oyuncu rüzgâra $43^\circ\text{–}45^\circ$ orsa seyrinde tramola (tacking) zamanlamasını, hamlelerde ağırlık atmayı (`Shift`) ve arkadan gelen dalga setlerinde sörf yapmayı kullanarak aynı rotayı %35–55 daha yüksek ortalama hızla tamamlar.
3. **Tohumlu Adalet ve Tam Belirlenimcilik (D-112):** Simülasyon durumu (`GelgitSimState`) işaretçisiz düz bellek bölgesinde (`src/sim/sim.h`) sabit $60\text{ Hz}$ ($\Delta t = 1/60\text{ s}$) tamsayı tick ile ilerler. Aynı tohum ve aynı girdi dizisi 1T ve 8T'de bit-bit aynı durumu ve 64-bit FNV-1a özetini üretir; hayalet yarış (Ghost Replay) tam olarak bu girdi akışını yeniden koşar.
4. **Sessiz Güzellik:** Minimal HUD (rüzgâr gülü, hız/sörf göstergesi, görev pusulası/durumu). Altın saat gün batımında turkuaz alt-yüzey saçılması (SSS) ve güneş yakamozu; fırtınada kurşuni-yeşil dev dalgalar ($H_s > 6\text{ m}$), yağmur perdeleri ve deniz feneri ışıkları.

---

## 2. Sayısal Fizik Modeli (Sim-Kritik, 60 Hz Deterministik)

### 2.1 Tekne Gövdesi ve Kütle Parametreleri
| Parametre | Sembol | Değer | Birim | Açıklama |
|---|---:|---:|---|---|
| Tam Boy | $L_{\text{OA}}$ | $7.85$ | $\text{m}$ | Kıç aynalık $z = -3.60\text{ m}$, pruva $z = +4.25\text{ m}$ |
| Azami En | $B$ | $3.04$ | $\text{m}$ | Vasat küpeşte genişliği ($\pm 1.52\text{ m}$) |
| Boş Kütle | $m_0$ | $680.0$ | $\text{kg}$ | Mürettebat + donanım dahil temel kütle |
| Kargo Kütlesi | $m_{\text{cargo}}$ | $0 \dots 160$ | $\text{kg}$ | Teslimat görevlerinde taşınan yük (atalet ve su çekimini artırır) |
| Baş-Kıç Ataleti | $I_{\text{pitch}}$ | $2400.0$ | $\text{kg}\cdot\text{m}^2$ | X ekseni etrafında açısal atalet |
| Yalpa Ataleti | $I_{\text{roll}}$ | $620.0$ | $\text{kg}\cdot\text{m}^2$ | Z ekseni etrafında açısal atalet |
| Dönüş Ataleti | $I_{\text{yaw}}$ | $2100.0$ | $\text{kg}\cdot\text{m}^2$ | Y ekseni etrafında açısal atalet |
| Metasantr Yüksekliği | $\text{GM}_0$ | $1.25$ | $\text{m}$ | Salma doğrultma kolu (Salma yükseltmesiyle $1.52\text{ m}$) |

### 2.2 Görünür Rüzgâr ve Yelken Aerodinamiği ($C_L / C_D$ Polar Tablosu)
- **Gerçek Rüzgâr ($\mathbf{V}_{\text{tw}}$):** Tohumlu hava cephesi + yerel rüzgâr hamlesi (gust):
  $$\mathbf{V}_{\text{tw}}(x, z, t) = \mathbf{V}_{\text{front}}(x, z, t) + \mathbf{V}_{\text{gust}}(x, z, t)$$
- **Görünür Rüzgâr ($\mathbf{V}_{\text{aw}}$):**
  $$\mathbf{V}_{\text{aw}} = \mathbf{V}_{\text{tw}} - \mathbf{V}_{\text{boat}}$$
- **Görünür Rüzgâr Açısı ($\beta_{\text{aw}} \in [-\pi, \pi]$):** Tekne pruva eksenine göre rüzgârın geliş açısı ($0 = \text{tam pruvadan / rüzgâr üstü}$, $\pi = \text{tam kıçtan / pupa}$).
- **İskota Açısı ($\delta_{\text{sail}}$):** Oyuncu `W/S` ile $\delta_{\text{sail}} \in [12^\circ, 80^\circ]$ ($0.21 \dots 1.40\text{ rad}$) aralığında ayarlar.
- **Etkin Hücum Açısı ($\alpha$):**
  $$\alpha = \max\left(0,\; |\beta_{\text{aw}}| - \delta_{\text{sail}}\right)$$
  $|\beta_{\text{aw}}| < 30^\circ$ (**ölü bölge / irons**) veya $|\beta_{\text{aw}}| < \delta_{\text{sail}} + 4^\circ$ olduğunda yelken boşa düşer (`luffing > 0`), yapraklar ve itki üretmez.

#### $C_L(\alpha)$ ve $C_D(\alpha)$ Parçalı Doğrusal Polar Tablosu (10 Düğüm)
Toplam yelken alanı $A_{\text{sail}} = 22.0\text{ m}^2$ (Ana yelken $13.5\text{ m}^2$ + Flok $8.5\text{ m}^2$), $\rho_{\text{air}} = 1.225\text{ kg/m}^3$, dinamik basınç $q = \frac{1}{2}\rho_{\text{air}} |\mathbf{V}_{\text{aw}}|^2$:

| Düğüm | Hücum Açısı $\alpha$ (°) | $C_L$ (Kaldırma) | $C_D$ (Sürükleme) | Seyir Rejimi |
|---:|---:|---:|---:|---|
| 0 | $0^\circ$ | $0.00$ | $0.05$ | Ölü bölge / yapraklama (Luffing) |
| 1 | $7^\circ$ | $0.62$ | $0.08$ | Dar orsa girişi |
| 2 | $16^\circ$ | $1.32$ | $0.14$ | **Optimum Orsa** ($\beta_{\text{tw}} \approx 42^\circ\text{–}45^\circ$, yüksek $L/D \approx 9.4$) |
| 3 | $26^\circ$ | $1.48$ | $0.25$ | **Apaz / Dar Apaz** (Maksimum aerodinamik kaldırma) |
| 4 | $40^\circ$ | $1.22$ | $0.48$ | Geniş apaz (Stall başlangıcı) |
| 5 | $60^\circ$ | $0.82$ | $0.85$ | Geniş apaz |
| 6 | $85^\circ$ | $0.38$ | $1.22$ | Pupa girişi |
| 7 | $115^\circ$ | $0.12$ | $1.34$ | **Pupa** (Paraşüt etkisi, sürükleme baskın) |
| 8 | $150^\circ$ | $0.05$ | $1.28$ | Tam pupa |
| 9 | $180^\circ$ | $0.00$ | $1.20$ | Ters akış |

- **Tekne Eksenlerinde Kuvvetler:**
  $$F_{\text{drive}} = q A_{\text{sail}} \left(C_L \sin|\beta_{\text{aw}}| - C_D \cos|\beta_{\text{aw}}|\right) \cos(\text{roll})$$
  $$F_{\text{heel}} = \text{sgn}(\beta_{\text{aw}}) \, q A_{\text{sail}} \left(C_L \cos|\beta_{\text{aw}}| + C_D \sin|\beta_{\text{aw}}|\right) \cos(\text{roll})$$

### 2.3 Salma, Dümen, Bayılma ve Alabora
- **Salma Yanal Direnci (Orsa Kabiliyeti):** Teknenin yanal hızı $v_{\text{lat}}$ salma tarafından yüksek sönümle karşılanır ($F_{\text{keel}} = -k_{\text{lat}} v_{\text{lat}} - k_{\text{lat2}} v_{\text{lat}}|v_{\text{lat}}|$). Bu sayede tekne rüzgâr altına kaymak yerine ileri gider ve gerçek rüzgâra **$42^\circ\text{–}45^\circ$ orsa açısıyla** rüzgâr üstüne tırmanabilir (VMG $> 0$).
- **İvmeli Dümen ve Input Buffer:** Dümen komutu $100\text{ ms}$ ($6\text{ tick}$) girdi tamponuyla yumuşatılır; dümen açısı $\delta_r \in [-0.55, +0.55]\text{ rad}$ sabit ivmeli ($\ddot{\delta}_r$) hareket eder.
- **Yatma (Heel) ve Ağırlık Atma (Hiking):**
  - Yelken yatma momenti $M_{\text{heel}} = F_{\text{heel}} \cdot h_{\text{ce}}$ ($h_{\text{ce}} = 3.15\text{ m}$).
  - Oyuncu `Shift` tuşuna bastığında mürettebat rüzgâr üstü küpeşteye çıkarak $\tau_{\text{hike}} = 1950\text{ N}\cdot\text{m}$ doğrultucu moment uygular.
- **Alabora (Capsize) Eşiği:** $|\text{roll}| \ge 58^\circ$ ($1.012\text{ rad}$) olduğunda tekne alabora olur (`state = BOAT_CAPSIZED`). Oyuncu 3 saniyelik kurtarma etkileşimiyle (`Shift`/`Space` + ters dümen) tekneyi doğrultabilir; oyun sona ermez ancak kargo varsa hasar alır.

### 2.4 Dalga Kaldırma Kuvveti ve Dalga Sörfü (Wave Surfing)
- **10 Gövde Örnekleme Noktası:** Pruva $(0, +3.6)$, kıç $(0, -3.3)$, iskele/sancak baş omuzluk $(\pm 0.95, +2.0)$, iskele/sancak ön vasat $(\pm 1.35, +0.6)$, iskele/sancak arka vasat $(\pm 1.35, -0.8)$, iskele/sancak kıç omuzluk $(\pm 1.05, -2.2)$.
- **Sim-Kritik Dalga Yüksekliği:** Her örnek noktasında deterministik JONSWAP dalga alanı örneklenir; batma derinliğinden Arşimet kaldırma kuvveti + düşey/açısal viskoz su sönümü hesaplanır.
- **Dalga Sörfü Koşulu ve İtkisi:**
  - Teknenin altındaki dalganın pruva yönündeki eğimi $s_{\text{wave}} = -\nabla h \cdot \hat{\mathbf{f}} > 0.055$ (dalga arkadan/omuzluktan gelip tekneyi ileri-aşağı eğiyor) ve tekne ileri hızı $v_{\text{fwd}} > 4.2\text{ m/s}$ olduğunda tekne dalga yüzeyine tutunur (`surf_factor` $\in [0, 1]$).
  - Dalga yerçekimi bileşeni $F_{\text{surf}} = m g \, s_{\text{wave}} \, \eta_{\text{surf}}$ ileri ivme sağlar ve hidrodinamik sürüklemeyi (planing etkisiyle) %28 azaltır.
  - Sonuç: Doğru açıyla yakalanan dalga sırtında tekne hızı düz/ortalama seyre göre **%35–%60 artar**.

---

## 3. Hava Cephesi, Fırtına ve Kedi Pençesi Hamleleri (Gusts)

- **Tohumlu Cephe Hattı:** Harita üzerinde $(\mathbf{p}_{\text{front}}, \hat{\mathbf{n}}_{\text{front}})$ ile tanımlı fırtına cephesi zamanla ilerler. Cephe önünde rüzgâr $8\text{–}11\text{ m/s}$ ($H_s \approx 1.8\text{–}2.4\text{ m}$), cephe içinde $18\text{–}24\text{ m/s}$ ($H_s \approx 6.0\text{–}7.5\text{ m}$).
- **Kedi Pençesi (Cat's Paw) Hamleleri:** Dünya uzayında hareket eden 8 adet dairesel rüzgâr hamlesi hücresi $(x_g, z_g, r_g, \Delta V_g)$. Tekneye ulaşmadan 2–3 saniye önce su yüzeyinde koyu pürüzlü dairesel bölge olarak görülür; içine girildiğinde rüzgâr hızı $+35\%\text{–}60\%$ sıçrar.

---

## 4. Takımada İçeriği, 6 Görev, "Günün Denizi" ve Yükseltmeler

### 4.1 Takımada Haritası (4 Ada, 2 Deniz Feneri, 1 Liman)
| Nokta | Konum $(x, z)$ (m) | Yarıçap (m) | Özellik |
|---|---|---|---|
| **1. Lodos Koyu (Liman Adası)** | $(0, -90)$ | $55$ | Ana liman iskelesi $(0, -32)$, kargo vinçleri, sıcak liman fenerleri |
| **2. Bozburun (Kuzey Fener Adası)** | $(20, 420)$ | $48$ | Sarp kayalık + **Kuzey Deniz Feneri** (yüksek ışık kulesi) |
| **3. Çapraz Ada (Doğu Ticaret Adası)** | $(380, 150)$ | $58$ | Doğal dalgakıranlı koy, teslimat iskelesi |
| **4. Kara Burun (Batı Fırtına Adası)** | $(-340, 220)$ | $52$ | Açık deniz kayalıkları + **Batı Deniz Feneri** |

### 4.2 6 Görev Akışı
1. **Görev 1 — Öğretici: Rüzgârı Oku ve İskota (`W/S`):** Rüzgâr yandan (apaz) eserken iskotayı optimum açıya getir, $6.5\text{ m/s}$ hıza çık ve $140\text{ m}$ ilerideki ilk şamandıraya ulaş.
2. **Görev 2 — Öğretici: Orsa ve Tramola (Tacking):** Tam rüzgâr üstündeki ($z = +240\text{ m}$) hedefe doğrudan gidilemez (ölü bölge). Rüzgâra $\sim 45^\circ$ açıyla sancak/iskele kontra zikzak (tramola) yaparak şamandıraya ulaş.
3. **Görev 3 — Öğretici: Dalga Sırtında Sörf:** İri okyanus dalgalarına rüzgâr altı / geniş apaz açısıyla gir, dalganın ön yüzünde sörfe kalk (`SURF`) ve hedef kapıya ulaş.
4. **Görev 4 — Öğretici: Sert Hamle ve Alabora Kurtarma:** Sert fırtına hamlesinde tekne aşırı yatarken `Shift` ile ağırlık at; alabora kurtarma tatbikatını tamamlayıp güvenli şamandıraya var.
5. **Görev 5 — Teslimat 1: Altın Saat Kargosu:** Lodos Koyu Limanı'ndan yükü al (`Space`), gün batımında Çapraz Ada iskelesine kargo bütünlüğünü koruyarak teslim et.
6. **Görev 6 — Teslimat 2: Fırtına Cephesi:** Yaklaşan fırtına cephesinde ($H_s > 6\text{ m}$, şiddetli rüzgâr ve hamleler) Çapraz Ada'dan Kuzey Bozburun Deniz Feneri'ne acil fener yakıtı ulaştır.

### 4.3 "Günün Denizi" (Daily Sea) ve Hayalet Yarış (Ghost Replay)
- Tohumu sabit (`0x47454C47`) 3 şamandıralı ada slalom parkuru.
- Oyuncunun her koşusu `SimReplay` ile kaydedilir; tamamlanan en hızlı koşu "En İyi Hayalet" olarak saklanır ve sonraki koşularda ikinci oyuncu/hayalet tekne olarak aynı deterministik simülasyon içinde yarışır.

### 4.4 3 Tekne Yükseltmesi (Görev Ödülleriyle Açılır)
1. **Yelken Kesimi (Yarış Bezi):** $C_L$ katsayısını $+15\%$ artırır, orsa açısını $3^\circ$ iyileştirir.
2. **Derin Kurşun Salma:** Doğrultma momentini ($\text{GM}$) $+22\%$ artırır, yanal kaymayı (leeway) azaltır.
3. **Bakır Kaplama Gövde:** Gövde sürüklemesini $-15\%$ düşürür, dalga sörfüne geçiş hızını kolaylaştırır.

---

## 5. Görsel Kumaş (`src/gfx3d/sail.*`), Ses ve Riskler

- **Yelken Kumaşı (`src/gfx3d/sail.*`):** Görsel sınıfta (D-112) $9 \times 9$ XPBD parçacık ağı; direk (luff) ve bumba (foot) kenarları pimli, görünür rüzgâr vektörü ve yapraklama (luffing) frekansıyla gerçek zamanlı şişer ve dalgalanır. Sim-kritik duruma geri yazmaz.
- **Sentez Sesler (`src/audio/*`):** Görünür rüzgâr hızına bağlı SVF filtrelenmiş rüzgâr uğultusu, gövde su hışırtısı, yelken boşa düştüğünde çırpınma (flapping), yüksek yatmada halat/ahşap gıcırtısı ve fırtınada gök gürültüsü.
- **Teknik Riskler ve Önlemler:**
  - *Risk 1 (GPU vs CPU Dalga Uyumu):* Sim-kritik kaldırma kuvveti CPU'da `ocean_sample` (veya deterministik çok-bileşenli JONSWAP dalga toplamı) kullanır; GPU ve CPU aynı JONSWAP spektrumunu paylaştığından görsel dalga ile fiziksel dalga birebir örtüşür.
  - *Risk 2 (1080p GPU < 10 ms Bütçesi):* Ada, deniz feneri, liman, şamandıra ve hayalet tekne statik/dinamik örnekleri mevcut `gpu_ocean` RHI boru hattına ek çizim çağrıları olarak sıfır ek geçişle (aynı MSAA 4x render pass içinde) eklenir.
