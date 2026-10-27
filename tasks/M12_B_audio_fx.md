DURUM: TODO
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
- [ ] `make check` hatasız/uyarısız (ASan/UBSan)
- [ ] Filtre: 1 kHz kesimde 200 Hz sinüs ≈ geçer (genlik ≥ 0.9), 8 kHz sinüs ≥ 18 dB bastırılır (ölçüm testi);
  rezonans 0.99'da bile kararlı (NaN/inf yok, 10 s gürültü girdisi)
- [ ] mix=0 ve cutoff=0 iken eski çıktıyla bit-bit aynı (aynı tohum, aynı olay dizisi)
- [ ] Reverb: dürtü yanıtı sönümlenir (2 s sonra |y| < 1e-3, room ≤ 0.9), çıktı [-1,1] (yumuşak kırpma korunur)
- [ ] Benchmark: 16 ses + reverb, 1 s ses için render süresi (ms, host) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/app/*`, `web/*`, `src/audio/audio.*` değişmez (entegrasyonu ben yapacağım). Öneriler Notlar'a.

## Notlar
