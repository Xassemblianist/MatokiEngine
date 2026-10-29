# M15_B — WebGPU temeli: RHI + komut akışı + JS yürütücü (D-108)

Durum: TODO

## Amaç
Motorun GPU temeli. C tarafında `src/gpu/rhi.h/.c`: tutamaç tabanlı kaynaklar (buffer, texture, sampler,
shader module (WGSL metni), render pipeline, bind group), kare başına komut akışı (begin_pass(clear renk/derinlik),
set_pipeline, set_bind_group, set_vertex/index_buffer, draw_indexed, write_buffer, write_texture, end_pass, present).
Akış paylaşımlı bellekte düz ikili biçim (sürüm numaralı başlık, komut kodu + uzunluk). `web/gpu.js` kare başına
akışı okuyup WebGPU çağrılarına çevirir; cihaz yoksa `engine_gpu_available()==0` ve motor CPU yoluna kalır.
Hatalar (WGSL derleme hatası, doğrulama hatası) wasm'a geri bildirilir (hata halkası). Arena dışında bellek yok.

Demo: `web/gpu_test.html` + C'de `SCENE_GPU_TEST` yerine bağımsız bir test girişi (main.c'ye dokunmadan
`engine_gpu_test_frame()` dışa aktarımı) — dönen renkli üçgen + dokulu, derinlik testli küp, elle yazılmış WGSL ile.

## Dosyalar
- `src/gpu/rhi.h`, `src/gpu/rhi.c`, `src/gpu/gpu_test.c` (yeni)
- `web/gpu.js`, `web/gpu_test.html` (yeni)
- `Makefile` (yalnız yeni kaynak dosyaları derlemeye eklemek gerekiyorsa)
- `tests/test_gpu_rhi.c` (komut akışı kodlama/çözme, sınır ve taşma testleri — host'ta GPU olmadan)
- `docs/ARCHITECTURE.md` §2 (komut akışı biçimi ve yeni dışa aktarımlar)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] Komut akışı biçimi ARCHITECTURE §2'de tam tanımlı; host testleri akışı kodlayıp bağımsız bir çözücüyle doğrular
- [ ] `gpu.js` bağımlılıksız saf ES modülü; motor mantığı içermez
- [ ] gpu_test.html'de üçgen + küp görünür (tarayıcıda doğrulama yöneticiye kalabilir; elle denetleyebildiğin
      kadarını Notlar'a yaz)
