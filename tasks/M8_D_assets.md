DURUM: DONE
HAT: 3D temel (tek ajan, 2026-10-27)

# M8_D — Varlık hattı: JSON, base64, OBJ/MTL, glTF 2.0/GLB, sürükle-bırak

Hepsi sıfırdan; libc yok, dış kütüphane yok (D-096).

## Dosyalar
- `src/asset/json.h/.c`, `base64.h/.c`, `obj.h/.c`, `gltf.h/.c`
- `src/app/scene3d.c/.h` (`scene3d_load_files`, model çizimi, HUD), `src/app/main.c` (`engine_file_*`,
  `engine_bench_json`, asset arena), `web/filedrop.js`, `web/platform.js` (kurulum çağrısı)
- `tests/test_asset_json.c`, `test_asset_obj.c`, `test_asset_gltf.c`, `tests/assets/make_assets.py` + üretilmiş varlıklar

## Kabul
- [x] `make check` 95/95
- [x] JSON: 47 geçerli + 57 geçersiz vaka (kontrol karakteri, kesik dize, vekil hataları, aşırı uzun UTF-8,
      derinlik sınırı), hata satır:sütun, Python `float()` ile 2000 sayı (tipik ≤1 ULP, tüm aralık ≤4 ULP,
      1393/2000 tam), ~1 MB rastgele belge gidiş-dönüş, 100.000 mutasyon fuzz (ASan) — çökme yok
- [x] OBJ: dörtgen küp (24 benzersiz köşe), negatif indeks + normal üretimi, v//vn, CRLF, hata satırı, MTL Kd/map_Kd, 20.000 mutasyon fuzz
- [x] glTF: gömülü base64 (iç içe byteStride, u16 indeks, TRS hiyerarşisi → dünya konumu doğrulandı, data: PNG),
      GLB (u32 indeks, normalized u16 UV, matrix node, bufferView PNG), u8 indeks + üretilen normal,
      3 bozuk dosya hata ile döner, 20.000 GLB bayt mutasyonu fuzz (ASan)
- [x] Tarayıcı: GLB küp ve 160k üçgenlik küre (1024² doku) sahneye yüklendi ve çizildi (ekran görüntüsü)

## Ölçümler (i7-10750H, Chrome 156, wasm -O2)
| | Boyut | Süre |
|---|---:|---:|
| JSON ayrıştırma (30.000 nesnelik dizi) | 4.26 MB | 18.5 ms (**230 MB/s**; tarayıcının yerel `JSON.parse`'ı 14.6 ms) |
| GLB yükleme (160k üçgen + 1024² PNG) | 4.6 MB | 28.6 ms = JSON 0.02 + PNG çöz + mip 21.5 + accessor/mesh 7.0 |
| OBJ yükleme (aynı küre, metin) | 15.5 MB | 114 ms (~135 MB/s) |
| 3D sahne + 160k üçgenlik model, 8T | — | 13.1 ms/kare (art arda ölçüm; bkz. M8_C uyarısı) |

## Ertelenenler
- JSON tam doğru yuvarlama (Ö-8), PBR/normal haritası/JPEG/KTX (Ö-9), çoklu UV seti, vertex rengi (COLOR_0), `.mmsh` ikili mesh formatı.
