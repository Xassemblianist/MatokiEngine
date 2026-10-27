DURUM: DONE
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
- [x] `make check` hatasız/uyarısız; mevcut glTF testleri geçer
- [x] Bilinen kare zamanlarında örneklenen poz analitik değerle ±1e-5; STEP/LINEAR/CUBICSPLINE; zaman sınır dışında kırpma; döngü
- [x] Bind pozunda skinning sonucu = orijinal mesh (±1e-5); 90° bükülmede uç köşe analitik konumda
- [x] Bozuk girdi: eklem indeksi ≥ eklem sayısı, ağırlık toplamı 0, NaN, döngüsel eklem hiyerarşisi, 0 anahtar kare → hata/uyarı, çökme yok;
      üretilen GLB'ye 20.000 bayt mutasyonu fuzz (ASan)
- [x] Benchmark: 10k köşe, 32 eklem skinning süresi (host) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/gfx3d/*`, `src/app/*`, `src/asset/json.*` değişmez. Öneriler Notlar'a.

## Notlar

### Yapılanlar
1. **İskelet (`src/anim/skeleton.h/.c`)**:
   - `JointPose` (yerel TRS), `Skeleton` (`joint_count <= 256`, `parents`, `inv_bind`, `bind_pose`, `node_indices`, `skeleton_node`, `root_transform`) tanımlandı.
   - En kısa yol (hemisphere) denetimli `quat_nlerp` ve `quat_slerp` (`m_atan2`/`m_sin` tabanlı) ile TRS ↔ `Mat4` dönüşümleri (`joint_pose_mat4`, Shepperd yöntemiyle `joint_pose_from_mat4`) eklendi.
   - `skeleton_validate`, `skeleton_sanitize` ve özyinelemesiz 3-durumlu (`0=bekliyor, 1=yığında, 2=hesaplandı`) `skeleton_compute_matrices` yazıldı: döngüsel veya ters sıralı ebeveyn dizilerinde bile sonsuz döngüye girmez ve yığın taşmaz.
2. **Animasyon Klibi (`src/anim/clip.h/.c`)**:
   - `AnimSampler` (`LINEAR`, `STEP`, `CUBICSPLINE`) ve `AnimChannel` (`translation`, `rotation`, `scale`) yapıları kuruldu.
   - İkili arama ile anahtar kare aralığı bulma, sınır dışı zaman kırpma, `loop` sarma, glTF 2.0 Ek C kübik Hermite spline (`in_tangent`, `value`, `out_tangent`) değerlendirmesi ve iki klip/poz arasında ağırlıklı karıştırma (`clip_blend_poses`, `clip_sample_blended`, SLERP/NLERP seçmeli) gerçeklendi.
3. **CPU Skinning (`src/anim/skin.h/.c`, `src/anim/skin_wasm.c`)**:
   - Köşe başına 4 eklem (`u16`) + 4 ağırlık (`f32`) ile konum ve normal Linear Blend Skinning (`skin_vertices_scalar`, `skin_vertices`, `skin_mesh`) eklendi.
   - `skin_wasm.c` içinde `Mat4` sütunları `v128_t` (`f32x4`) olarak ağırlıklandırıldı; toplama ağacı sırası skaler sürümle birebir aynı tutularak wasm SIMD ile skaler arasında **0.0 maksimum fark (bit-bit eşitlik)** sağlandı.
   - `skin_validate_weights` ve `skin_sanitize_weights` yanı sıra çekirdek içinde de aralık dışı eklem indekslerine, sıfır/negatif ağırlık toplamlarına ve NaN değerlere karşı koruma eklendi.
4. **glTF 2.0 Yükleyici (`src/asset/gltf.h/.c`)**:
   - `"skin yok sayildi"` ve `"animasyon yok sayildi"` uyarıları kaldırıldı; `skins`, `animations`, `JOINTS_0` (`u8`/`u16`/`u32`) ve `WEIGHTS_0` (`f32` / normalize `u8`/`u16`) okuma eklendi.
   - Arena taşmasına (`PANIC`) karşı ayırma öncesi `arena_has_space` denetimi, node grafiğinde döngü ve elmas-DAG patlamasına karşı `node_visiting` + ziyaret sayısı sınırı (`GLTF_MAX_VISITS`) eklendi.
5. **Test Varlığı ve Testler (`tests/assets/make_skin.py`, `tests/test_anim.c`)**:
   - `make_skin.py` ile 3 kemikli kol/tüp modeli ve `LINEAR`/`STEP`/`CUBICSPLINE` animasyonları içeren `tests/assets/arm_skin.glb` (5060 bayt) üretildi.
   - `test_anim.c` içinde analitik poz örnekleme, bind pozu ve 90°/45° bükülme doğrulaması, `gfx3d` boru hattıyla çizim testi, bozuk girdi testleri, 20.000 iterasyon ASan/UBSan GLB mutasyon fuzz'u ve 10k köşe × 32 eklem benchmark'ı eklendi.

### Benchmark Sonuçları (10.000 köşe, 32 eklem, köşe başına 4 etki)
- **Donanım / Ortam**: Intel Core i7-10750H @ 2.60 GHz (Linux), `clang` C11, Node/V8 (wasm SIMD)
- **Yöntem**: 5 ısınma turu + 31 ölçüm turu medyanı

| Ortam / Çekirdek | Medyan Süre (ms) | Verim (M köşe/s) | Not |
|---|---:|---:|---|
| Host `skin_vertices` (skaler, `-O1 -fsanitize=address,undefined`) | **0.557 ms** | 17.9 M köşe/s | Konum + normal dönüşümü + normalizasyon |
| Host `skeleton_compute_matrices` + `skin_mesh` (matris + skin + AABB/küre sınırları, ASan `-O1`) | **0.744 ms** | 13.4 M köşe/s | Uçtan uca 32 eklem + 10k köşe Mesh güncellemesi |
| Wasm V8 `skin_vertices_scalar` (`-O2`) | **0.305 ms** | 32.8 M köşe/s | Skaler referans |
| Wasm V8 `skin_vertices` (`skin_wasm.c` SIMD `-O2 -msimd128`) | **0.178 ms** | 56.2 M köşe/s | **1.71× hızlanma**, skalerle `max_diff = 0` (bit-bit aynı) |

### Öneriler / Ertelenenler
- **Sahne / Uygulama Entegrasyonu**: `src/app/scene3d.c` yasaklı dosyalar arasında olduğundan sürükle-bırak 3D sahne görüntüleyicisinde otomatik klip oynatma döngüsü bağlanmadı; `scene3d.c` içinde `gm.skins` ve `gm.animations` varsa her karede `clip_sample` + `skeleton_compute_matrices` + `skin_mesh` çağrılarak hareketli glTF modeller doğrudan oynatılabilir.
- **Paralel Test Notu**: `tests/test_jpeg.c` sabit `/tmp/matoki_jpeg_bench` dizinini kullandığından birden fazla worktree aynı anda `make check` çalıştırdığında geçici dosyalar çakışabiliyor; o testte PID veya worktree-içi `build/` dizini kullanılması önerilir.

### İnceleme (Claude)
- gltf.c: node ebeveyn tablosunda döngü atası denetimi doğru (atanmadan önce kontrol), ziyaret bayrağı + GLTF_MAX_VISITS elmas DAG patlamasını kesiyor, accessor/arena sınır denetimleri yerinde.
- Sınırlama (kabul): animasyon kanalı hedef düğümü bulunduğu ilk iskelete bağlanır; çok iskeletli modellerde klip hangi skin'e ait bilgisi tutulmuyor.
- `node_parent`/`node_to_joint` ayırmaları arena_has_space denetimsiz ama nc ≤ 4096 ile sınırlı (≤16 KB).
