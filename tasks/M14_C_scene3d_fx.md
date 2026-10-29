# M14_C — 3D görüntüleyiciye parçacık + ses efektleri

Durum: DONE

## Amaç
M12_B (TPT SVF filtre, `synth_set_reverb`) ve M12_C (`gfx3d/billboard`, `gfx3d/particles`) motorda var ama hiçbir
sahnede kullanılmıyor. scene3d'de göster:
- Sahnede sürekli bir ateş/duman yayıcısı + P tuşuyla kıvılcım patlaması (vcolor veya yeni küçük bir parçacık
  shader'ı ile alfa harmanlı, arkadan öne sıralı). Ertelenmiş g3d modu (D-105) açık: billboard mesh'i g3d_flush'a
  kadar değişmemeli.
- Patlamada filtreli (cutoff kaymalı) ses + hafif yankı.
- HUD'a parçacık sayısı ve parçacık güncelleme/mesh süresi.

## Dosyalar
- `src/app/scene3d.c/.h`, `src/app/r3d.c/.h` (emissive/saydam çizim kancası gerekiyorsa), `shaders/` (yeni .mtk ise)
  + `src/app/shaders.c` (gömülü shader listesi ve C referansı)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Parçacıklar JIT ve C referansta aynı görünür; kare süresine eklenen maliyet ölçülüp Notlar'a yazılır
- [x] Kuyu sahnesine dokunma

## Notlar
- **Parçacık shader'ı (`shaders/particle3d.mtk` & `src/app/shaders.c`)**:
  - Dallanmasız yumuşak dairesel sönümleme (`t = clamp(1.0 - (dx*dx + dy*dy), 0.0, 1.0); w = t*t;`) ve derinlik bazlı mesafe sisi (`f = clamp(depth * fog.a * 0.6, 0.0, 1.0)`) uygulayan `particle3d.mtk` eklendi (`uniform fog: vec4;`, `in uv: vec2; in v_color: vec4; in depth: f32;`).
  - `src/app/shaders.c` içerisine birebir aynı aritmetiği çalıştıran `ref_shade_particle3d` C referans fonksiyonu ve `"particle3d.mtk"` eşleşmesi eklendi.
  - JIT derlemesi (`MTK_FUSE_ALPHA` dahil): `0.240 ms` derleme süresi, `544 byte` Wasm kodu, `44 IR` komutu (`16 byte` uniform, `7` varying).
- **Saydam parçacık çizim kancası (`src/app/r3d.h/.c`)**:
  - `R3dPass` yapısına `G3dMaterial particles` eklendi (`BLEND_ALPHA`, `G3D_ATTR_UV`, `G3D_ATTR_COLOR`, `G3D_ATTR_VIEW_DEPTH`, `cull = G3D_CULL_BACK`, `depth_test = TRUE`, `depth_write = FALSE`).
  - `r3d_draw_particles(R3dPass* p, const Mesh* mesh)` gölge geçişinde (`shadow_only`) erken döner, ana geçişte birim model matrisiyle çizim komutu kaydeder.
- **Sahne entegrasyonu, D-105 uyumu ve ses efektleri (`src/app/scene3d.h/.c`)**:
  - `PARTICLE_CAP = 4096` kapasiteli `ParticleSystem` ve `BillboardBatch` kalıcı arenada ayrıldı; sahne açılışında ateş/dumanın ilk kareden görünmesi için `1.5 sn` (`30 × 0.05 sn`) ön-ısıtma yapıldı.
  - Sol-ön bölgede `(-3.2, 0.25, 1.5)` konumunda sürekli ateş (`90/sn`) ve duman (`55/sn`) yayıcısı (`emit_continuous_fx`) eklendi.
  - `P` tuşu (`KEY_P = 80`) ile kıvılcım patlaması (`trigger_spark_burst`: `240` kıvılcım + `40` duman/alev) tetiklenir; aynı anda `synth_set_reverb(s, 0.22f, 0.65f, 0.40f)` ile hafif yankı açılır ve üstel cutoff kaymalı iki TPT SVF ses efekti çalınır (`k_sfx_burst_boom`: `WAVE_NOISE`, `2800 -> 180 Hz`, `res = 0.45`; `k_sfx_burst_spark`: `WAVE_SAW`, `3600 -> 320 Hz`, `res = 0.60`).
  - Ertelenmiş `g3d` modu (D-105) kurallarına uygun olarak `particles_update` ve arkadan öne radix sıralamalı `particles_build_billboards(..., TRUE)` çağrıları `r3d_render`'dan **önce** tamamlanır, `draw_scene` sonunda `r3d_draw_particles` çağrılır ve `r3d_render` içindeki `g3d_flush` bitene kadar `g_billboards.mesh` tamponlarına dokunulmaz.
  - HUD'un 2. satırına `[P] Patlama | Parcacik <aktif>/<kapasite> | sim+mesh <toplam> ms (sim <güncelleme> + mesh <inşa>)` bilgisi ve profilleyiciye `particles` sayacı eklendi.
- **JIT vs C-Ref doğrulaması ve Benchmark ölçümleri (Node.js v22.21.1, Wasm SIMD, 1T, `tod = 0.25` PBR + gölge, 10 ısınma + 61 kare medyan)**:
  - **Görsel eşdeğerlik (`dt = 0` sabit karede 3D viewport pikselleri)**:
    - Sürekli ateş+duman (`185` aktif parçacık, parçacıksız sahneye göre `3908` piksel değişimi): JIT ve C-Ref çıktıları **bit-exact aynı** (`0` farklı piksel, `maxChanDiff = 0`).
    - Ateş+duman + `P` kıvılcım patlaması (`465` aktif parçacık): JIT ve C-Ref çıktıları **bit-exact aynı** (`0` farklı piksel, `maxChanDiff = 0`).
  - **Ses doğrulaması (`44100 Hz`)**: `P` patlaması tetiklendiğinde üretilen `3087` ses örneğinin `3086` tanesi sıfırdan farklı (`maxAbsSample = 0.3902`, `0` NaN/Inf).
  - **Kare süresi maliyeti**:
    | Senaryo | Aktif Parçacık | Kare Medyan (ms) | `particles` sim+mesh (ms) | `shade` ort. (ms) | `raster` ort. (ms) | `r3d_flush` ort. (ms) |
    | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
    | Parçacıksız baz (JIT) | 0 | 15.739 | 0.000 | 6.977 | 3.572 | 9.733 |
    | Sürekli ateş+duman (JIT) | ~185 | 16.463 (+0.724) | 0.032 | 7.277 (+0.300) | 3.977 (+0.405) | 10.422 (+0.689) |
    | Sürekli ateş+duman (C-Ref) | ~185 | 41.545 | 0.030 | 31.593 | 29.118 | 35.537 |
    | Ateş+duman + `P` patlama (JIT) | ~465 | 16.973 (+1.234) | 0.057 | 7.491 (+0.514) | 4.211 (+0.639) | 10.896 (+1.163) |
