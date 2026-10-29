# M17_A — Gerçekçi okyanus sahnesi (FFT dalgaları + yüzen tekne)

Durum: TODO

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
- [ ] `make check` hatasız + uyarısız
- [ ] Testler: FFT ↔ naif DFT, periyodiklik, spektrum varyansı → Hs beklenen aralıkta (U=10 m/s), belirlenimcilik
- [ ] JIT ↔ C referans görüntüsü eşit (HUD hariç); başsız Chrome ekran görüntüsü `build/ocean_shot.png`
- [ ] 640×360'ta 8T kare süresi ölçülüp Notlar'a yazılır
