# M15_A — Matoki → WGSL kod üretici (D-108)

Durum: DONE (argon)

## Amaç
Aynı `.mtk` kaynağından WebGPU fragment shader'ı (WGSL) üret. Matoki IR'ını (optimizasyon sonrası) girdi al;
skaler/SPMD anlam aynı kalsın (CPU'da quad başına 4 lane, GPU'da piksel başına bir çağrı). Uniform'lar tek bir
uniform buffer (std140/WGSL hizalama kuralları, MtkLayout ile uyumlu ofsetler belgelensin), dokular
`texture_2d<f32>` + `sampler` bind group'u, varying'ler `@location(n)`. `sample`/mip, `dFdx/dFdy` (→ `dpdx/dpdy`),
`discard`, trigonometri ve tüm yerleşikler. Ayrıca gfx3d varying sözleşmesine uyan basit bir WGSL vertex shader
üretici (mvp + model + öznitelikler) — G3dAttr listesine göre.

## Dosyalar
- `src/matoki/wgsl.c`, `src/matoki/wgsl.h` (yeni), `src/matoki/matoki.h` (yeni API bildirimi)
- `tests/test_matoki_wgsl.c` (yeni)
- `docs/MATOKI.md` (WGSL bölümü)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] `shaders/` altındaki 9 shader'ın hepsi WGSL'e çevriliyor; testte üretilen metin sözdizimi denetiminden geçer
      (kendi küçük denetleyicin: parantez/blok dengesi, bildirilen tüm tanımlayıcılar tanımlı, @group/@binding tekil)
- [x] Üretilen WGSL'ler `build/wgsl/*.wgsl` olarak yazdırılabilir (test veya host aracı) — tarayıcıda doğrulanacak
- [x] Bilinmeyen/desteklenmeyen IR komutu → derleme hatası (sessiz yanlış kod yok)

## Notlar
- **Yapılanlar**:
  - `src/matoki/wgsl.h` ve `src/matoki/wgsl.c` eklendi (`src/matoki/matoki.h` üzerinden açıklandı). Libc ve `malloc` kullanmadan, tamamen yığın/tampon tabanlı (`MtkWgslBuf`) C11 WGSL kod üreticisi yazıldı.
  - `mtk_wgsl_frag`: Optimizasyon sonrası `IrFunc` ve isteğe bağlı `MtkLayout` girdisinden `@fragment fn frag(fin: FragIn) -> @location(0) vec4<f32>` üretir:
    - **Uniform tamponu**: `@group(0) @binding(0) var<uniform> mtk_u: MtkUniforms` (`data: array<vec4<f32>, N>`). `IR_UNIFORM` bayt ofseti `slot = off / 16`, bileşen `(off / 4) & 3` (`.x/.y/.z/.w`) olarak doğrudan indekslenir; böylece `MtkLayout` bayt ofsetleriyle bire bir uyumludur ve WGSL 16-bayt uniform dizi elemanı hizalama kısıtını doğal olarak karşılar.
    - **Doku + sampler bağlamaları**: Her doku yuvası `s` için `@group(0) @binding(1 + 2*s) var tex_s: texture_2d<f32>` ve `@group(0) @binding(2 + 2*s) var samp_s: sampler`.
    - **Varying eşlemesi**: `MtkLayout` verildiğinde bildirilen tip/isimlerle (`vec2<f32>`, `vec3<f32>`, `vec4<f32>`, `f32`) `@location(0..K-1)` alanları oluşturulur; `MtkLayout` `NULL` ise skaler `@location(i) v_i: f32` kullanılır.
    - **Doku örnekleme (`sample` / `sample_level`)**: `lower.c`'nin `ir_emit_sample` ile ürettiği `IR_TEX_W`, `IR_TEX_H`, `IR_TEX_LEVELS`, `IR_DERIV_X/Y` (`dpdx`/`dpdy`) ve `IR_TEXEL_LOAD` zinciri WGSL'de `textureDimensions(tex_s, 0)`, `textureNumLevels(tex_s)` ve `bitcast<i32>(pack4x8unorm(textureLoad(tex_s, vec2<i32>(wx, wy), level)))` olarak üretilir (`wy * stride_s + wx` deseni tanınarak 2B koordinat ve mip seviyesi geri çıkarılır; genel 1B indeks için `idx % w, idx / w` yedek yolu çalışır). Böylece `ir_opt_alpha_cmp` gibi paketli RGBA8 tam sayı optimizasyonları ve sarmalama/filtre semantiği CPU/Wasm arka ucuyla bit-bire-bir korunur.
    - **Kontrol akışı ve kapsam**: `IR_IF_BEGIN` / `IR_ELSE` / `IR_IF_END` blokları WGSL `if (cond) { ... } else { ... }` olarak üretilir. Blok içinde tanımlanıp blok kapandıktan sonra (`IR_SELECT` vb. ile) kullanılan SSA değerleri fonksiyon başında `var t<id>: <tip> = <sıfır>;` olarak önceden bildirilir. İçinde `dpdx`/`dpdy` barındıran koşullu bloklar WGSL uniform kontrol akışı kuralını ihlal etmemek için düzleştirilir (`IR_SELECT` semantiği korunur). `IR_DISCARD` → `if (cond) { discard; }`.
    - **Hata denetimi**: `ir_verify` kontrolüne ek olarak geçersiz/bilinmeyen IR opcode, hizasız uniform ofseti, sınır dışı varying/doku/SSA referansı, kapanmamış `IF` bloğu, eksik çıkış kanalı veya tampon taşması durumlarında anlamlı hata mesajıyla `0` döner (sessiz yanlış kod üretilmez).
  - `mtk_wgsl_vert`: `gfx3d/pipeline.h` varying sözleşmesiyle (`MtkWgslAttr`: `UV=2`, `NORMAL=3`, `DEPTH=1`, `WORLD_POS=3`, `SHADOW_POS=3`) uyumlu `@vertex fn vert` üretir (`@group(1) @binding(0)` altında `mvp`, `model`, `normal_mat`, `shadow_mat`).
  - `mtk_compile_wgsl`: `.mtk` kaynağından tek adımda `parse → sema → lower → opt → mtk_wgsl_frag` çalıştıran üst seviye yardımcı.
  - `tests/test_matoki_wgsl.c`: Kendi WGSL sözdizimi/kapsam/bağlama denetleyicimizi (`wgsl_validate_syntax`: parantez/köşeli/süslü/açısal ayraç dengesi, `@group/@binding` ve `@location` tekilliği, sözcüksel kapsam ağacında tüm tanımlayıcıların bildirilmiş olması, yerleşik fonksiyon/tip tablosu) ve 5 kapsamlı birim testini içerir.
  - `docs/MATOKI.md`: `§11 WGSL Kod Üretimi (WebGPU Arka Ucu, D-108)` bölümü eklendi.
- **Gözlenen sonuçlar (`build/wgsl/*.wgsl`)**:
  - `shaders/` altındaki 9 shader'ın tamamı (`solid`, `vcolor`, `sprite`, `plasma`, `branchy`, `unlit3d`, `lit3d`, `dungeon3d`, `pbr3d`) ve `pbr3d_vs` vertex shader'ı WGSL'e çevrilip `wgsl_validate_syntax` denetiminden geçti ve `build/wgsl/` altına yazıldı:
    - `solid.wgsl`: 23 satır, 593 bayt
    - `vcolor.wgsl`: 32 satır, 884 bayt
    - `branchy.wgsl`: 56 satır, 1526 bayt
    - `unlit3d.wgsl`: 64 satır, 1919 bayt
    - `sprite.wgsl`: 68 satır, 2182 bayt
    - `plasma.wgsl`: 195 satır, 6566 bayt
    - `lit3d.wgsl`: 219 satır, 7841 bayt
    - `dungeon3d.wgsl`: 330 satır, 11417 bayt
    - `pbr3d.wgsl`: 553 satır, 19941 bayt
    - `pbr3d_vs.wgsl`: 39 satır, 992 bayt
    - Toplam: 1579 satır, 53861 bayt (`make check`: 173 test geçti, 0 hata, 0 uyarı).
