# Sonnet düzeltme promptu (kısa)

```text
MATOKI motoru (/home/ostriquetrum/WEBASSEMBLY, C11 → wasm + WebGPU). Önce AGENTS.md'yi oku; kurallara uy, tüm metinler Türkçe.

SORUN: Tarayıcıda (http://localhost:8080, `python3 -m http.server`'a benzer sunucu zaten açık olabilir) CPU yolundaki sahneler bozuk:
- 6:Oyun (2D) bembeyaz, sadece HUD var; 7:Ocak duvarlar düz renk + siyah; 9:Okyanus su ve gökyüzü siyah, yalnız tekne görünüyor;
  5:3D zemin siyah. HUD "CPU 640x360" diyor (WebGPU yolu da devreye girmiyor olabilir).
- Commit 8802e8c'de (R0 merge) okyanus düzgündü (gün batımı, su, köpük).

ŞÜPHELİLER (8802e8c..HEAD): T1 (src/matoki/opt.c constfold değişikliği), G1_A/G1_B/T2 (src/gpu, web/gpu.js, src/app/main.c),
yönetici düzeltmeleri (src/app/shaders.c wasm_cap 64K→256K, src/matoki/variant.c), testsiz merge'ler: OYUN1_gelgit + P1_F_gpu_fluid
(main.c sahne kaydı: SCENE_GELGIT=9 tuş 0, SCENE_AKISKAN=10 tuş '-'; SCENE_COUNT=11 — dizilerin/g_mode_ms vb. boyutunu kontrol et).

YAPILACAK:
1. Sorunu tekrarla: headless Chrome ile `node tools/host/webgpu_check.mjs <url> <ifade.js>` (ekran görüntüsü build/webgpu_shot.png).
   Sahne değiştirme için klavye olayı yetmiyorsa üst çubuğa tıkla ya da wasm export'u (engine_* sahne seçimi) çağır.
   Konsoldaki shader derleme hatalarına/panik mesajlarına bak; J tuşu (Matoki JIT ↔ C referans) farkı da ipucu.
2. `git bisect` ile 8802e8c..HEAD arasında bozan commit'i bul (her adımda `make` + ekran görüntüsü). En az değişiklikle düzelt;
   kök nedeni bul, semptomu yamalama.
3. Doğrula: 1–9, 0, '-' sahnelerinin hepsinin ekran görüntüsü (CPU yolu; WebGPU varsa GPU yolu da) — gerçekten bak.
   `make check` temiz.
4. main'e commit et (mesaj Türkçe), sonra `/home/ostriquetrum/matoki-wt/queue/publish.sh` ile GitHub'a gönder.
   Kısa Türkçe rapor: kök neden, düzeltme, ekran görüntüleri.
Kapsam dışı: yeni özellik ekleme, yarım dallara (V1_sky_clouds, P1_A_phys2_switch, V2_terrain) dokunma.
```
