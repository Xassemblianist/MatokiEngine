DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M9_E — Sahne / ECS çekirdeği ve kaydetme-yükleme (sıfırdan)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, libc YOK (`src/` altında).
JSON için mevcut `src/asset/json.h` kullanılır (değiştirilmez). Bellek `core/arena.h` (malloc yok).

## Tasarım
- Entity = 32 bit {indeks:20, nesil:12}; silinen entity'nin eski tutamağı geçersizleşir (nesil denetimi).
- Bileşen depoları: tip başına seyrek küme (sparse set) — yoğun dizi + seyrek indeks; ekleme/silme O(1),
  yoğun dizi yineleme önbellek dostu. Bileşen tipi çalışma zamanında kayıtlı (`ecs_register(world, "Transform", size, align, &desc)`),
  alan tanımları (isim, tip: f32/i32/u32/b32/vec3/vec4/quat/entity/str_id, ofset) serileştirme için.
- Sorgu: `EcsQuery q = ecs_query(world, mask)` → en küçük depoyu sürer, diğerlerini denetler; yineleme sırasında
  silme ertelenir (komut tamponu).
- Hiyerarşi: Parent bileşeni + yerel/dünya Transform (konum, quat, ölçek) → dünya matrisi güncelleme sistemi
  (`core/math.h` Mat4), döngü denetimi.
- Kaydet/yükle: tüm dünya → JSON (bileşen alan tanımlarına göre), JSON → dünya; entity referansları yeniden eşlenir.
  Bilinmeyen bileşen/alan uyarı olarak atlanır.

## Dosyalar (YALNIZCA bunlar)
- `src/scene/ecs.h/.c`, `src/scene/transform.h/.c`, `src/scene/scene_io.h/.c`
- `src/asset/json_writer.h/.c` (yalnız gerekirse: json.h'de yazıcı varsa onu kullan; json.c'ye dokunma)
- `tests/test_ecs.c`
- `tasks/M9_E_scene_ecs.md`

## Kabul kriterleri
- [x] `make check` hatasız, uyarısız (ASan/UBSan)
- [x] Nesil: sil-yeniden oluştur sonrası eski tutamak `ecs_alive` = false; 1M rastgele ekle/sil/bileşen işlemi
      bir model (basit dizi tabanlı referans uygulaması, test içinde) ile her adımda tutarlı
- [x] Sorgu sonuçları referans modelle aynı küme; yineleme içinde silme güvenli
- [x] Hiyerarşi: 3 seviyeli zincirde dünya matrisi elle hesaplanana ±1e-5; ebeveyn silinince çocuk politikası
      (birlikte silinir) doğrulanır; döngü oluşturma reddedilir
- [x] Kaydet → yükle → kaydet: iki JSON birebir aynı; entity referansları doğru eşlenir
- [x] Benchmark (host): 100k entity, 3 bileşen; tam sorgu yineleme süresi (ns/entity) ve 10k ekle/sil süresi Notlar'a

## Yasaklar
- `docs/*`, `Makefile`, `src/asset/json.c/.h`, `src/app/*`, başka modüller değişmez. Öneriler Notlar'a.

## Notlar

### Yapılanlar
- **`src/scene/ecs.h/.c`**:
  - 32-bit `EcsEntity` (`{indeks:20, nesil:12}`). Nesil sayacı `1..4095` aralığında döner (`0` atlanır), böylece `ECS_NULL = 0` hiçbir zaman geçerli bir canlı tutamakla çakışmaz (4100+ ardışık sil-oluştur döngüsüyle doğrulandı).
  - Tip başına seyrek küme (`EcsStore`): `sparse[ent_cap]` (`u32`), `dense_ent[dense_cap]` ve hizalı `data[dense_cap * stride]` (`Arena` üzerinden). Silme işlemi swap-and-pop ile O(1) zamanda çalışır.
  - `ecs_query(world, mask)`: maskedeki bileşen depolarından eleman sayısı (`count`) en küçük olanı sürücü (`driver`) olarak seçer, diğer bileşenleri entity maskesi üzerinden O(1) denetler.
  - Yineleme sırasında çağrılan `ecs_destroy` ve `ecs_remove` işlemleri `iter_depth > 0` iken komut tamponuna (`EcsCmd`) ertelenir; en dıştaki sorgu tamamlandığında `ecs_flush` ile otomatik uygulanır.
  - `ECS_FIELD_STR_ID` alanları için dünya arenasında dize internleme tablosu (`ecs_str_intern`, `ecs_str_get`) eklendi.
- **`src/scene/transform.h/.c`**:
  - `Quat` yardımcıları (`quat_norm`, `quat_mul`, `quat_from_axis_angle`, `quat_rotate_vec3`), `Transform` (`pos`, `rot`, `scale`, `world`) ve `Parent` (`parent`) bileşenleri.
  - `transform_set_parent`: ata zincirini yürüyerek kendi kendine veya çok adımlı döngü oluşturacak atamaları `FALSE` ile reddeder.
  - `transform_update`: `visit_state` (0: ziyaret edilmedi, 1: yığında, 2: tamamlandı) ile bellek içi DFS (memoization) kullanarak yoğun dizi sırasından bağımsız şekilde her düğümün `world` matrisini tek geçişte günceller.
  - Ebeveyn silindiğinde çocuk ve torun entity'ler kademeli olarak birlikte silinir (`ecs_destroy_immediate`).
- **`src/scene/scene_io.h/.c`**:
  - `src/asset/json.h` içindeki mevcut `json_parse` ve `json_write` doğrudan kullanıldı (`src/asset/json_writer.h/.c` dosyasına gerek kalmadı).
  - `scene_save`: canlı entity'lere deterministik `0..N-1` sıra kimliği verir; tüm 9 alan tipini (`f32`, `i32`, `u32`, `b32`, `vec3`, `vec4`, `quat`, `entity`, `str_id`) geçici arenada `JsonValue` ağacına dönüştürüp yazar.
  - `scene_load`: 2 geçişli yükleme (1. geçişte entity'leri oluşturup eski ID → yeni `EcsEntity` tablosunu kurar, 2. geçişte ileri/geri tüm entity referanslarını yeni tutamaklara eşler). Bilinmeyen bileşen ve alanlar çökmeden `SceneIoResult.warnings` sayacını artırarak atlanır.

### Benchmark Sonuçları
Ölçüm ortamı: **Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz**, Linux, `clang` (5 ısınma turu + 25 tekrar ortalaması, 3 koşunun medyanı).
Senaryo: 100.000 entity, 3 bileşen (`CompPos` 12 B, `CompVel` 12 B, `CompStats` 8 B).

| Ölçüm | Host `-O1 -fsanitize=address,undefined` (`make check`) | Host `-O2` (sanitizer'sız) |
|---|---:|---:|
| 100k entity, 3 bileşen tam sorgu (`ecs_query_get`) | 4.052 ms (**40.52 ns/entity**) | 1.108 ms (**11.08 ns/entity**) |
| 100k entity, 3 bileşen tam sorgu (`ecs_get`) | 4.547 ms (**45.47 ns/entity**) | 1.067 ms (**10.67 ns/entity**) |
| 10k entity sil + yeniden oluştur + 3 bileşen ekle | 2.253 ms (**225.3 ns/entity**) | 0.434 ms (**43.4 ns/entity**) |
| 10k tekil bileşen sil + ekle (`ecs_remove` + `ecs_set`) | 0.576 ms (**57.6 ns/işlem**) | 0.165 ms (**16.5 ns/işlem**) |

### Ertelenenler / Öneriler
- Arketip (archetype) veya grup (group) tabanlı çoklu bileşen hizalama: Çok bileşenli sorgularda tüm bileşenlerin yoğun dizilerini aynı sırada tutan "owning group" yapısı ileride >1M entity senaryolarında dolaylı `sparse` erişimini tamamen kaldırabilir.
- İkili (binary) sahne serileştirme biçimi: Büyük sahnelerde JSON metnine ek olarak doğrudan bellek görüntüsü/ikili akış desteği eklenebilir.

## İnceleme (Claude, 2026-10-27)
- `transform_update` özyinelemeliydi → derin zincirde yığın taşması; açık yığınlı (`EcsWorld.walk_stack`) sürüme çevrildi.
  Transform'suz ebeveynin `visit_state`'i kalıcı 2 kalıyordu (indeks yeniden kullanılınca yeni entity atlanabilirdi) → yalnız Transform'lu entity'ler işaretlenir.
- `scene_load` dünyayı temizledikten sonra yapı hatasıyla dönebiliyordu → doğrulama temizlemeden önce.
- `ecs_register` alan ofseti + boyut ≤ bileşen boyutu ASSERT'i.
- Yeni test `ecs_review_deep_chain_and_load_safety`: 100k derinlikli zincir, Transform'suz ara ebeveyn, bozuk yüklemede dünya korunur.
- Not: `transform_set_parent` döngü denetimi zincir derinliği kadar yürür (yukarıdan aşağı derin zincir kurmak O(n²)); şimdilik kabul.
