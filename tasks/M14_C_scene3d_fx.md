# M14_C — 3D görüntüleyiciye parçacık + ses efektleri

Durum: TODO

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
- [ ] `make check` hatasız + uyarısız
- [ ] Parçacıklar JIT ve C referansta aynı görünür; kare süresine eklenen maliyet ölçülüp Notlar'a yazılır
- [ ] Kuyu sahnesine dokunma
