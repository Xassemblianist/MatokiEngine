# Yol Haritası

Her milestone sonunda **çalışan, ölçülmüş** bir şey olmalı.

| # | Milestone | Çıktı |
|---|---|---|
| M0 | İskelet | Makefile, platform katmanı, wasm'ın doldurduğu framebuffer canvas'ta animasyonlu |
| M1 | Çekirdek | math, rng, log/printf, profiler, native test koşucusu (`make test`) |
| M2 | Referans raster | Skaler sabit-nokta üçgen rasterizer, dokulu quad, alpha blend, 10k sprite demo + benchmark |
| M3 | mbin + JIT köprüsü | wasm binary üretici, paylaşılan tablo, C'den elle üretilmiş bir fonksiyonu JIT'leyip çağırma |
| M4 | Matoki v0 | lexer→parser→sema→IR→codegen, `sprite.mtk` JIT'lenir, C referansıyla eşdeğerlik testi |
| M5 | Hız | SIMD raster, uniform özelleştirme, blend füzyonu, elle WAT vs clang yarışı, benchmark tablosu |
| M6 | Thread'ler | worker'lar, SAB, karo kuyruğu, JIT modül çoğaltma |
| M7 | 2D motor | tilemap, kamera, sprite animasyon, ses synth'i → küçük bir 2D oyun ✅ (M7_0..M7_3) |
| M8 | 3D | derinlik, clipping, perspektif-doğru interpolasyon, mipmap ✅ (M8_A, M8_B); model yükleme sırada |
| M9 | Çatallar | `games/blokca` (voxel), 3D sahne, opsiyonel WebGPU arka ucu |

Görev dosyaları: `tasks/M<milestone>_<sıra>_<isim>.md`
