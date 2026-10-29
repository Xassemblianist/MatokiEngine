# P1_F — GPU akışkan / parçacık fiziği (PBF)

Durum: TODO

## Amaç
Matoki compute ile Position Based Fluids (Macklin 2013): uzaysal hash ızgarası (sayma sıralaması, atomic_add), yoğunluk
kısıtı, vortisite, XSPH viskozite; 256k parçacık. Ekran-uzayı akışkan çizimi (derinlik yumuşatma + normal + kırılma).
Yeni sahne ya da 5:3D içinde tuşla açılan "kova" demosu; rijit cisimlerle tek yönlü etkileşim. Görsel sınıf (D-112): GPU, belirlenimci değil.

## Dosyalar
`shaders/compute/*` (yeni), `src/gpu/*`, `web/gpu.js`, `src/app/scene_fluid.*` (yeni), `src/app/main.c`, `src/matoki/*` (yalnız eksik compute özelliği), `tests/*`

## Kabul kriterleri
- [ ] 256k parçacık GPU ms (timestamp-query) Notlar'da, hedef < 6 ms
- [ ] Ekran görüntüsü build/ altında, gözle kontrol
- [ ] make check + make gpucheck temiz
