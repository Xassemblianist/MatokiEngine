# Yol Haritası

Her milestone sonunda tarayıcıda **çalışan, gösterilebilir** bir şey olmalı.

| # | Milestone | Çıktı |
|---|---|---|
| M0 | İskelet | Makefile, platform katmanı, wasm'dan renk değişen ekran, input okunuyor |
| M1 | İlk küp | math lib, kamera, komut tamponu, WebGPU'da dönen doku kaplı küp |
| M2 | Tek chunk | blok kaydı, prosedürel doku atlası, culled mesher, uçan kamera |
| M3 | Sonsuz dünya | kendi simplex noise'umuz, biyomlar, mağaralar, ağaçlar, chunk streaming |
| M4 | Oynanış | AABB fizik, yerçekimi, DDA raycast, kır/koy, hotbar, bitmap font |
| M5 | Işık | gök + blok ışığı flood-fill, smooth AO, gece/gündüz, sis |
| M6 | Su & saydamlık | sıvı simülasyonu, saydam geçiş, sıralama, sualtı efekti |
| M7 | Kayıt | region formatı, kendi LZ77 sıkıştırmamız, IndexedDB |
| M8 | Çoklu thread | worker'lar, SAB, iş kuyruğu, gen+mesh paralel |
| M9 | Hayat | ses synth'i, envanter, crafting, mob'lar (A* yol bulma) |
| M10 | Saçmalık | redstone benzeri mantık devreleri, yazılım rasterizer modu ("Hayalet GPU") |

Görev dosyaları: `tasks/M<milestone>_<sıra>_<isim>.md`
