# M15_B — WebGPU temeli: RHI + komut akışı + JS yürütücü (D-108)

DURUM: DONE

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
- [x] `make check` hatasız + uyarısız
- [x] Komut akışı biçimi ARCHITECTURE §2'de tam tanımlı; host testleri akışı kodlayıp bağımsız bir çözücüyle doğrular
- [x] `gpu.js` bağımlılıksız saf ES modülü; motor mantığı içermez
- [x] gpu_test.html'de üçgen + küp görünür (tarayıcıda doğrulama yöneticiye kalabilir; elle denetleyebildiğin
      kadarını Notlar'a yaz)

## Notlar
- **Tutamaç ve kaynak yönetimi (`src/gpu/rhi.h`, `src/gpu/rhi.c`)**:
  - 32-bit nesil sayaçlı tutamaç kodlaması (`(type << 24) | (gen << 16) | index`, `0 = geçersiz`) ile 6 kaynak türü (`BUFFER`, `TEXTURE`, `SAMPLER`, `SHADER`, `PIPELINE`, `BIND_GROUP`) yönetildi.
  - Tüm bellek (`RhiResourceSlot` tablosu ve 16-bayt hizalı komut akışı tamponu) `Arena` üzerinden ayrılır; `malloc` veya `libc` kullanılmaz.
  - Kaynak yok edildiğinde yuva serbest bırakılır, yeniden tahsiste `gen` sayacı artırılarak eski (bayat) tutamaçların ve yanlış tip tutamaçların tüm RHI çağrılarında `O(1)` sürede reddedilmesi sağlandı.
- **İkili komut akışı ve güvenlik (`docs/ARCHITECTURE.md` §2)**:
  - 16 baytlık `RhiStreamHeader` (`magic = 0x4D475055 "MGPU"`, `version = 1`, `byte_len`, `cmd_count`) ve ardından 8 baytlık `RhiCmdHeader` (`opcode`, `size`) ile başlayan 19 komut tanımlandı ve tüm yapı boyutları `_Static_assert` ile kilitlendi.
  - Değişken uzunluklu veriler (`CREATE_SHADER` WGSL kaynağı, `WRITE_BUFFER` ve `WRITE_TEXTURE` yükleri) komutun hemen ardından akış içine gömülüp 4 bayt katına sıfırla tamamlandı; böylece C yığınındaki (stack) geçici veriler kare sonunda JS yürütücüsü çalışana kadar güvenle taşınır ve akış tamamen kendi kendine yeterlidir.
  - Akış kapasitesi dolduğunda yarım komut yazılmaz (`rhi->overflow = TRUE`), kaynak oluşturma komutu akışa sığmazsa kaynak yuvası sızdırılmaz ve akışın önceki kısmı %100 geçerli kalır.
- **Hata halkası (`RhiErrorRing`)**:
  - Paylaşımlı bellekte 16 girdilik (`RhiErrorEntry`, 256 B) tek üretici / tek tüketici halka (`write_pos`, `read_pos`, `capacity`, `dropped`) kuruldu; `web/gpu.js` WGSL derleme (`getCompilationInfo`) ve doğrulama (`pushErrorScope("validation")` / `uncapturederror`) hatalarını `Atomics` ile yazar, C tarafı `rhi_pop_error` / `rhi_poll_errors` ile tüketir.
- **Demo ve doğrulama ölçümleri (`src/gpu/gpu_test.c`, `web/gpu.js`, `web/gpu_test.html`, `tests/test_gpu_rhi.c`)**:
  - `engine_gpu_test_frame(now_ms, dt)` `main.c`'ye dokunmadan ilk karede derinlik dokusu (`640×360 DEPTH24_PLUS`), dönen renkli üçgen ve `mesh_cube` + `64×64` prosedürel dama dokulu (`tex_gen_checker`) 3D küp kaynaklarını oluşturur; sonraki karelerde yalnız uniform tamponlarını güncelleyip tek render geçişinde her iki nesneyi indeksli çizer.
  - Host testlerinde (`tests/test_gpu_rhi.c`, 7 test) bağımsız ikili akış çözücüsü ile tüm 19 komut, tutamaç nesil/tip güvenliği, buffer/doku sınır denetimleri, akış taşması (kanarya bayt kontrolü), bozuk akış reddi ve hata halkası doğrulandı.
  - Gerçek `build/engine.wasm` + `web/gpu.js` (`SharedArrayBuffer` üzerinde) Node.js ortamında sahte WebGPU cihazıyla uçtan uca koşturularak doğrulandı:
    - Cihaz yokken `engine_gpu_available() == 0` döndüğü, cihaz varken `1` olduğu,
    - 1. karede `20116` bayt / `35` komut (15 GPU kaynağı + `16384` B doku yüklemesi + 2 `drawIndexed` çağrısı: 3 ve 36 indeks) üretilip yürütüldüğü,
    - 2. ve sonraki karelerde kararlı durumda `528` bayt / `15` komut üretildiği ve `1000` kare kodlama + bağımsız çözme süresinin host ASan/O1 derlemesinde toplam `2.412 ms` (`2.412 µs/kare`) olduğu ölçüldü.
    - Tarayıcıda gerçek GPU üzerinde görsel doğrulama (`web/gpu_test.html`) yöneticiye bırakılmıştır.
