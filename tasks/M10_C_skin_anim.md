DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M10_C — İskelet animasyonu: glTF skin + animasyon yükleme, örnekleme, CPU skinning

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turdaki incelemelerde bozuk/düşmanca girdi hataları bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle.

## Hedef
glTF 2.0'daki `skins` (joints, inverseBindMatrices, skeleton) ve `animations` (channels/samplers: translation, rotation,
scale; LINEAR, STEP, CUBICSPLINE) okunur. Poz örnekleme (quat slerp/nlerp, iki animasyon arasında ağırlıklı karıştırma),
eklem hiyerarşisinden dünya matrisleri, köşe başına 4 eklem/ağırlık ile **CPU skinning** (konum + normal) — çıktı sıradan
`Mesh` (gfx3d mevcut hattı çizer).

## Dosyalar (YALNIZCA bunlar)
- `src/asset/gltf.h/.c` — skin/animasyon/JOINTS_0/WEIGHTS_0 okuma eklenir (mevcut davranış ve testler bozulmaz;
  "skin yok sayildi" uyarıları kaldırılır). Accessor okumaları mevcut sınır denetimli yolları kullanır.
- `src/anim/skeleton.h/.c`, `src/anim/clip.h/.c` (örnekleme, karıştırma), `src/anim/skin.h/.c` (CPU skinning; isteğe bağlı `skin_wasm.c` SIMD, skalerle ≤1e-5 aynı)
- `tests/test_anim.c`, `tests/assets/make_skin.py` (yalnız Python stdlib: 2-3 kemikli bükülen bir kol/tüp modeli + animasyon, GLB) + üretilen dosya
- `tasks/M10_C_skin_anim.md`

## Kabul
- [ ] `make check` hatasız/uyarısız; mevcut glTF testleri geçer
- [ ] Bilinen kare zamanlarında örneklenen poz analitik değerle ±1e-5; STEP/LINEAR/CUBICSPLINE; zaman sınır dışında kırpma; döngü
- [ ] Bind pozunda skinning sonucu = orijinal mesh (±1e-5); 90° bükülmede uç köşe analitik konumda
- [ ] Bozuk girdi: eklem indeksi ≥ eklem sayısı, ağırlık toplamı 0, NaN, döngüsel eklem hiyerarşisi, 0 anahtar kare → hata/uyarı, çökme yok;
      üretilen GLB'ye 20.000 bayt mutasyonu fuzz (ASan)
- [ ] Benchmark: 10k köşe, 32 eklem skinning süresi (host) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/gfx3d/*`, `src/app/*`, `src/asset/json.*` değişmez. Öneriler Notlar'a.
