# M17_A — Gerçekçi okyanus sahnesi (FFT dalgaları + yüzen tekne)

Durum: DONE

## Amaç
Yeni sahne `SCENE_OKYANUS` ("9:Okyanus"): Tessendorf FFT okyanus, Kuyu'dan çok daha etkileyici bir vitrin.
- `src/gfx3d/ocean.c/.h`: 3 kademe (ör. L = 500 / 60 / 8 m, N = 128/128/64), JONSWAP spektrumu (rüzgâr hızı, fetch,
  γ=3.3) + yönlü yayılım (cos², karşı yöne küçük pay), derin su dağılımı ω=√(gk); kademe sınırlarında k aralığı
  kesilir (çift sayım yok). Kare başına kademe başına 4 karmaşık 2B IFFT (iki gerçek alan tek IFFT'de): h, Dx, Dz,
  ∂h/∂x, ∂h/∂z, ∂Dx/∂x, ∂Dz/∂z, ∂Dx/∂z. FFT: radix-2, önceden twiddle + bit ters; satır/sütun `job_parallel_for`.
  Köpük: Jacobian J = (1+λDx_x)(1+λDz_z) − λ²Dx_z², J < eşik → köpük; zamanla sönen birikim.
- Mesh: kameraya ortalı, uzaklıkla geometrik büyüyen aralıklı ızgara (ufka kadar), merkez kaba aralığa yakalanır;
  köşe yer değiştirmesi CPU'da kademelerden bilinear örneklenir, yüksek kademeler uzaklıkla söner.
- Kademe başına RGBA8 eğim+köpük dokusu (@bilinear @mipmap, her kare `tex_build_mips`) → piksel başına normal.
- `shaders/ocean.mtk` + C referansı: Schlick Fresnel, gökyüzü yansıması (analitik ufuk/zenit + güneş), güneş parlaması,
  dalga tepesinde alt yüzey saçılması (yükseklik × arka ışık), köpük, mesafe sisi, tonemap. `shaders/sky.mtk` gök kubbesi.
- Yüzen tekne: 6–8 noktadan su yüksekliği örneklenen kaldırma kuvveti, sönümlü yalpa/baş-kıç/dalış; orbit kamera.
- Tuşlar (rakam tuşları sprite sayısına bağlı, kullanma): rüzgâr ±, dalgalılık (choppiness) ±, zaman ölçeği, tel kafes.
- HUD: FFT ms, mesh ms, shade ms, Hs (anlamlı dalga yüksekliği).

## Dosyalar
- `src/gfx3d/ocean.*` (yeni), `src/app/scene_ocean.*` (yeni), `shaders/ocean.mtk`, `shaders/sky.mtk` (yeni),
  `src/app/shaders.c/.h` (C referansları + kayıt), `src/app/main.c` (sahne kaydı), `tests/test_ocean.c` (yeni)
- Fikir kaynağı: SamurayBalgami/Matoki `src/phys/ocean.c` (izinli, okunabilir).

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Testler: FFT ↔ naif DFT, periyodiklik, spektrum varyansı → Hs beklenen aralıkta (U=10 m/s), belirlenimcilik
- [x] JIT ↔ C referans görüntüsü eşit (HUD hariç); başsız Chrome ekran görüntüsü `build/ocean_shot.png`
- [x] 640×360'ta 8T kare süresi ölçülüp Notlar'a yazılır

## Notlar

### Yapılanlar
1. **3 Kademeli Tessendorf FFT Okyanus (`src/gfx3d/ocean.h`, `src/gfx3d/ocean.c`)**:
   - Üç ayrık dalga sayısı kademesi: Kademe 0 (`L = 500 m, N = 128`, kaba okyanus soluğanı), Kademe 1 (`L = 60 m, N = 128`, orta rüzgâr dalgaları), Kademe 2 (`L = 8 m, N = 64`, ince kapiler/kısa dalgalar). Kademe sınırlarında `[k_min, k_max)` kesimiyle enerji çift sayımı önlendi.
   - **JONSWAP Spektrumu**: Hasselmann (1973) boyutsuz fetch bağıntıları ($\alpha, \omega_p$), Pierson-Moskowitz temel şekli, $\gamma = 3.3$ doruk güçlendirme çarpanı, $\cos^2\theta$ yönlü yayılım + karşı yöne küçük enerji payı (`swell_opp`), derin su dispersiyonu $\omega(k) = \sqrt{gk}$ ve Box-Muller Gauss genlikleri (`Rng` ile deterministik).
   - **Paralel 2B IFFT**: Önceden hesaplanmış bit-tersleme ve twiddle tablolarıyla radix-2 kelebek FFT; iki gerçek Hermityen alanı (`f1 + i·f2`) tek karmaşık IFFT'de birleştirerek kademe başına 4×2B IFFT ile 8 gerçek alanı (`h, Dx, Dz, ∂h/∂x, ∂h/∂z, ∂Dx/∂x, ∂Dz/∂z, ∂Dx/∂z`) çıkaran yapı. Satır ve sütun geçişleri `job_parallel_for` ile işçi iş parçacıklarına dağıtıldı.
   - **Jacobian Köpük & Mipmap Dokusu**: $J = (1+\lambda\partial_x D_x)(1+\lambda\partial_z D_z) - \lambda^2(\partial_z D_x)^2 < J_{\text{eşik}}$ kırılma kriteriyle zamanla sönen beyaz başlık (`foam`) birikimi; her kademe için RGBA8 (`R=∂h/∂x, G=∂h/∂z, B=h, A=köpük`) dokusu paketlenip her karede `tex_build_mips` ile mipmap zinciri üretildi.
   - **Kamera Merkezli Geometrik Izgara (`ocean_mesh_init`, `ocean_mesh_update`)**: `96×96` (9.409 köşe, 18.432 üçgen, 320 m yarıçap) kamera merkezli, kaba ızgara adımına (`2.5 m`) yakalanan (yüzme/titreme yapmayan) ve kenarlara doğru üstel/geometrik büyüyen ızgara. Köşe güncellemesi de `job_parallel_for` ile paralel çalışır.
2. **Matoki Gölgelendiricileri ve C Referansları (`shaders/ocean.mtk`, `shaders/sky.mtk`, `src/app/shaders.c/.h`)**:
   - `ocean.mtk`: 3 kademeli mipmap'li eğim+köpük dokularından piksel başına yüzey normali, Schlick Fresnel ($F_0 = 0.02$), analitik gökyüzü yansıması, çok loblu güneş yakamozu (`nh16 + nh64 + nh256`), dik dalga tepelerinde yerel yükseklik ve arka ışıkla modüle edilen turkuaz-zümrüt alt yüzey saçılması (SSS), Jacobian köpüğü, mesafe sisi ve ACES (Narkowicz) + gama 2 ton eşleme.
   - `sky.mtk`: Rayleigh zenit-ufuk gradyanı, Mie ileri saçılma halesi, keskin güneş diski, trigonometrisiz yüksek irtifa sirüs bulutları ve ACES + gama 2.
   - `ref_shade_ocean` ve `ref_shade_sky`: Matoki JIT derleyicisinin ürettiği Wasm koduyla **HUD hariç 217.600 / 217.600 pikselde (%100, `maxChannelDiff = 0`) bit-birebir aynı** çıktı üretir.
3. **Yüzen Yelkenli Tekne, Kontroller ve HUD (`src/app/scene_ocean.c/.h`, `src/app/main.c`)**:
   - Çok parçalı ahşap yelkenli (V-karinalı gövde, güverte, ışıklı kamara, ana direk, bumba, civadra, kırmızı flama, rüzgârla şişkin ana yelken ve flok yelkeni).
   - Gövde altındaki 8 su altı prob noktasından `ocean_sample` ile su yüksekliği, yüzey normali ve yatay dalga hızı örneklenerek Arşimet kaldırma kuvveti, dalga eğimi doğrultucu momenti ve hidrodinamik sönümleme ile gerçekçi dalış/baş-kıç/yalpa (`heave/pitch/roll`) fiziği.
   - Orbit (`CAM_ORBIT`) ve serbest uçuş (`CAM_FLY`, `C` tuşu) kameraları; `U/I` (veya Orbit'te Yukarı/Aşağı ok) rüzgâr hızı, `H/K` (veya Sol/Sağ ok) dalgalılık (`choppiness`), `L/O` + `Space` zaman ölçeği/duraklatma, `F/G` güneş yüksekliği, `M`/`V` tel kafes (`Kapalı` / `Karma` / `Tel`), `R` sıfırlama.
   - 2 satırlık alt HUD: `FFT ms`, `Mesh ms`, `Shade ms`, `Hs` (ölçülen ve teorik $4\sqrt{m_0}$), çizilen üçgen sayısı ve aktif kontrol özetleri.
4. **Birim Testleri (`tests/test_ocean.c`)**:
   - `ocean_fft_matches_naive_dft`: 1B ve 2B radix-2 FFT/IFFT'nin doğrudan $O(N^2)$ naif DFT ile $< 10^{-4}$ hata içinde örtüşmesi.
   - `ocean_cascade_periodicity`: Kademe sınırlarında $h(x+L, z) = h(x, z)$ periyodikliği ve ayrık $k$ aralıkları.
   - `ocean_jonswap_variance_and_hs`: $U_{10} = 10\text{ m/s}$ için teorik ve örneklenen $H_s = 4\sqrt{\sigma_h^2}$ değerlerinin beklenen $[1.5\text{ m}, 2.8\text{ m}]$ aralığında ($H_s \approx 2.17\text{ m}$, teorik $2.14\text{ m}$) çıkması ve rüzgâr hızıyla monoton artışı.
   - `ocean_determinism_and_foam`: Aynı tohum ve zamanda bit-birebir deterministik çıktı + yüksek dalgalılıkta Jacobian köpük üretimi.
   - `ocean_shaders_compile`: `ocean.mtk` ve `sky.mtk` JIT derlemesi ve JIT ↔ C referans eşleşmesi.

### Performans Ölçümleri (640×360, 8 İş Parçacığı / 8T, Başsız Chrome)
- **Makine yükü (`uptime`)**: `load average: 1.21, 1.53, 1.70` (Linux, 8 Web Worker / `8T`).
- **Görüntü doğrulaması (`build/ocean_shot.png`)**: Üst bar (18 px) ve alt HUD (38 px) dışındaki **217.600 pikselin 217.600'ü (%100.0) JIT ve C referansı arasında bit-birebir aynıdır** (`maxChannelDiff = 0`).

| Mod (640×360, 8T) | Kare Ort. (ms) | Kare Medyan (ms) | FFT Ort. (ms) | Mesh Ort. (ms) | Shade Ort. (ms) | Çizilen Üçgen | $H_s$ (Ölçülen / Teorik) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Matoki JIT (`ocean.mtk` + `sky.mtk`)** | **11.56 ms** (~86.5 FPS) | **11.52 ms** | 6.00 ms | 0.18 ms | **2.47 ms** | 4.211 | 2.17 m / 2.14 m |
| **C Referans (`ref_shade_ocean` + `ref_shade_sky`)** | **16.73 ms** (~59.8 FPS) | **16.82 ms** | 6.00 ms | 0.18 ms | **6.88 ms** | 4.211 | 2.17 m / 2.14 m |

- Matoki Wasm SIMD JIT gölgelendiricileri (`ocean.mtk` + `sky.mtk`), C referans gölgelendiricilerine kıyasla gölgeleme (`shade`) aşamasını **2.78× hızlandırmaktadır** (`6.88 ms → 2.47 ms`).
