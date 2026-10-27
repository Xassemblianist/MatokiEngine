DURUM: TODO
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
- [ ] `make check` hatasız, uyarısız (ASan/UBSan)
- [ ] Nesil: sil-yeniden oluştur sonrası eski tutamak `ecs_alive` = false; 1M rastgele ekle/sil/bileşen işlemi
      bir model (basit dizi tabanlı referans uygulaması, test içinde) ile her adımda tutarlı
- [ ] Sorgu sonuçları referans modelle aynı küme; yineleme içinde silme güvenli
- [ ] Hiyerarşi: 3 seviyeli zincirde dünya matrisi elle hesaplanana ±1e-5; ebeveyn silinince çocuk politikası
      (birlikte silinir) doğrulanır; döngü oluşturma reddedilir
- [ ] Kaydet → yükle → kaydet: iki JSON birebir aynı; entity referansları doğru eşlenir
- [ ] Benchmark (host): 100k entity, 3 bileşen; tam sorgu yineleme süresi (ns/entity) ve 10k ekle/sil süresi Notlar'a

## Yasaklar
- `docs/*`, `Makefile`, `src/asset/json.c/.h`, `src/app/*`, başka modüller değişmez. Öneriler Notlar'a.
