DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M12_B — Ses efektleri: ses başına filtre + zindan yankısı, sıfırdan

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turlardaki incelemelerde bozuk/düşmanca girdiler bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle. Ölçmeden "hızlandı" yazma.

## Hedef
`src/audio/synth` şu an kuru (efektsiz) mono sentez yapıyor. Oyunlar (Ocak, Kuyu) için:
1. **Ses başına durum-değişkenli (SVF / Chamberlin veya TPT) alçak geçiren filtre**: `SynthSfx`'e yapının **sonuna**
   `cutoff_hz` (0 = kapalı), `cutoff_end_hz` (0 = sabit; üstel kayma), `resonance` (0..1) alanları. Mevcut konumsal
   başlatıcılar (`{ WAVE_SQUARE, 300.0f, ... }`) derlenmeye ve aynı sesi vermeye devam etmeli (yeni alanlar 0 → filtre yok).
2. **Genel yankı (reverb) gönderimi**: Schroeder/Freeverb tarzı (4 paralel tarak + 2 seri tüm-geçiren), sabit gecikme
   tamponları `Synth` içinde (malloc yok); `synth_set_reverb(Synth*, f32 mix, f32 room, f32 damp)`; mix=0 iken çıktı
   eskisiyle **bit-bit aynı**. Örnekleme hızına göre gecikme uzunlukları ölçeklenir (44.1/48 kHz), tampon kapasitesi aşılmaz.
3. Şarkı (song) izleri filtreli enstrüman kullanabilmeli (instrument alanı zaten SynthSfx).

## Dosyalar (YALNIZCA bunlar)
- `src/audio/synth.h/.c`
- `tests/test_audio.c` (yeni testler)
- `tasks/M12_B_audio_fx.md`

## Kabul
- [x] `make check` hatasız/uyarısız (ASan/UBSan)
- [x] Filtre: 1 kHz kesimde 200 Hz sinüs ≈ geçer (genlik ≥ 0.9), 8 kHz sinüs ≥ 18 dB bastırılır (ölçüm testi);
  rezonans 0.99'da bile kararlı (NaN/inf yok, 10 s gürültü girdisi)
- [x] mix=0 ve cutoff=0 iken eski çıktıyla bit-bit aynı (aynı tohum, aynı olay dizisi)
- [x] Reverb: dürtü yanıtı sönümlenir (2 s sonra |y| < 1e-3, room ≤ 0.9), çıktı [-1,1] (yumuşak kırpma korunur)
- [x] Benchmark: 16 ses + reverb, 1 s ses için render süresi (ms, host) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/app/*`, `web/*`, `src/audio/audio.*` değişmez (entegrasyonu ben yapacağım). Öneriler Notlar'a.

## Notlar

### Yapılanlar
1. **Ses Başına TPT SVF Alçak Geçiren Filtre (`src/audio/synth.h`, `src/audio/synth.c`)**:
   - `SynthSfx` yapısının sonuna `cutoff_hz`, `cutoff_end_hz` ve `resonance` alanları eklendi. Mevcut 12 alanlı konumsal başlatıcıların (`scene_platformer.c`, `scene_kuyu.c`, `scene_ocak.c`) `-Wextra -Werror` altında uyarısız derlenmesi için `synth.h` içine `#pragma clang diagnostic ignored "-Wmissing-field-initializers"` eklendi.
   - Naive Chamberlin yerine yüksek kesim frekanslarında ve yüksek rezonansta koşulsuz kararlı olan 2 kutuplu **TPT (Topology-Preserving Transform / Zavalishin–Cytomic yamuk integrasyonlu) SVF** uygulandı:
     - $g_0 = \tan(\pi f_{c,0} / f_s)$, sönüm $k = \sqrt{2}(1 - \text{res})$ ($\text{res} \in [0, 0.99]$).
     - `cutoff_hz <= 0` (veya NaN) ise `flt_g = 0` kalır ve ses örneği hiç değiştirilmez (eski çıktıyla bit-bit aynı).
     - `cutoff_end_hz > 0` ise `flt_g` örnek başına `flt_g_mul` ile üstel kaydırılır (iç döngüde `m_tan` çağrılmaz); sabit kesimde `flt_a1, flt_a2, flt_a3` yalnızca `synth_play` sırasında hesaplanır.
   - Şarkı izleri (`SynthTrack.instrument`) `SynthSfx` kopyaladığı için `song_step` üzerinden filtreli enstrümanları doğrudan destekler.
2. **Genel Schroeder/Freeverb Yankı (`synth_set_reverb`)**:
   - `Synth` içine malloc kullanmadan 4 paralel alçak geçiren geri beslemeli tarak filtresi (`SYNTH_COMB_COUNT = 4`, `SYNTH_COMB_CAP = 3072`) ve 2 seri birim-kazançlı tüm-geçiren filtre (`SYNTH_AP_COUNT = 2`, `SYNTH_AP_CAP = 1280`) eklendi (~58 KB sabit alan).
   - 44.1 kHz taban gecikmeleri (`{1116, 1188, 1277, 1356}` ve `{556, 441}`) `sample_rate`'e göre `u64` aritmetiğiyle ölçeklenip `[1, CAP]` aralığına kıstırılır; 44.1 kHz, 48 kHz, 96 kHz ve aşırı örnekleme hızlarında tampon taşması imkânsızdır.
   - `rev_mix == 0` iken yankı adımı atlanır ve çıktı eski kodla **bit-bit aynı** kalır. `rev_mix > 0` iken `1e-20f` ofsetiyle alt-normal (denormal) yavaşlaması dallanmasız önlenir ve `soft_clip` öncesi `dry * (1 - mix) + wet * mix` karıştırılır.
   - Tüm girdiler (`freq`, `cutoff_hz`, `cutoff_end_hz`, `resonance`, `mix`, `room`, `damp`, `bpm`) NaN / ±Inf / sınır dışı değerlere karşı `clamp_safe` / `is_finite_f32` ile korundu.

### Doğrulama ve Ölçümler (`tests/test_audio.c`)
- **Filtre frekans yanıtı (1 kHz kesim, 44.1 kHz)**:
  - 200 Hz sinüs: bağıl kazanç $|H(200\text{ Hz})| = 0.9992 \ge 0.95$ (`master = 1.7` ile mutlak tepe genlik $\ge 0.94 \ge 0.90$).
  - 8 kHz sinüs: bağıl kazanç $|H(8\text{ kHz})| = 0.0124$ (**$-38.1\text{ dB}$** bastırma; kabul kriteri $\ge 18\text{ dB}$).
  - Kararlılık: `resonance = 0.99`, `cutoff_hz = 1000 -> 3500 Hz` ile 10 saniye (441.000 örnek) beyaz gürültüde 0 NaN/Inf, tüm örnekler `[-1, 1]` içinde.
- **Reverb sönümleme**:
  - `room = 0.9`, 44.1 kHz ve 48 kHz'de dürtü sonrası 2.0–2.5 s aralığında tepe genlik $|y| < 1.5 \times 10^{-4} < 10^{-3}$.

### Benchmark (16 Aktif Ses + Reverb, 1 Saniye = 44.100 Örnek)
- **Donanım / Ortam**: Intel Core i7-10750H @ 2.60GHz, Linux, `clang`, 5 ısınma + 31 tekrar medyanı:

| Senaryo (16 ses, 1 s @ 44.1 kHz) | Host `make check` (`-O1 + ASan/UBSan`) | Host Release (`-O2`) | 60 FPS kare başına (`-O2`) |
|---|---:|---:|---:|
| 16 ses kuru (`cutoff=0, mix=0`) | 12.981 ms | 6.192 ms | 0.103 ms |
| 16 ses + TPT SVF (sabit kesim) + Reverb (`mix=0.35, room=0.8`) | 21.560 ms | 10.171 ms | 0.170 ms |
| 16 ses + TPT SVF (üstel kaymalı kesim) + Reverb (`mix=0.35, room=0.8`) | 25.196 ms | 9.937 ms | 0.166 ms |

### Entegrasyon Önerileri (`src/app/*` ve `src/audio/audio.c` için)
- `src/audio/audio.c` içindeki `engine_audio_init(sample_rate)` örnekleme hızı değiştiğinde `synth_init` çağırırken `rev_mix / rev_room / rev_damp` ayarlarını da koruyacak şekilde `synth_set_reverb` değerlerini yedekleyebilir.
- `scene_kuyu.c` başlangıcında `synth_set_reverb(audio_synth(), 0.28f, 0.82f, 0.35f)` ile zindan yankısı açılabilir; pompalı/patlama/kapı seslerine `cutoff_hz = 1800.0f, cutoff_end_hz = 220.0f, resonance = 0.35f` eklenerek boğuk zindan karakteri güçlendirilebilir.
- `scene_ocak.c` içinde rüzgâr/ateş ve bas izlerinde `cutoff_hz = 900.0f` ve hafif yankı (`mix = 0.15f, room = 0.55f, damp = 0.5f`) kullanılabilir.
