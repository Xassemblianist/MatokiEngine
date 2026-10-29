# M16_A — Deterministik simülasyon çekirdeği + rollback (netcode temeli)

Durum: DONE (argon)

## Amaç
Vizyonun 2. sütunu: tarayıcıda gecikmesiz çok oyunculu oyun için motorun parçası olan rollback çekirdeği
(ağ katmanı sonraki iş; bu görev ağsız).
- `src/sim/sim.h/.c`: sabit adımlı simülasyon döngüsü (ör. 60 Hz), oyuncu başına girdi halkası (kare numaralı),
  tahmini girdi (son girdiyi tekrarla), gecikmeli gerçek girdi gelince **geri sar + yeniden simüle et**.
- Durum anlık görüntüsü: oyun durumu tek bir arena bölgesinde tutulur; snapshot = o bölgenin memcpy'ı (halka,
  ör. 16 kare). İşaretçi yerine ofset/indeks kullanımı belgelensin.
- Durum özeti (hash, ör. FNV-1a/xxhash benzeri, kendi yazımın) ile senkron kaybı tespiti.
- Belirlenimcilik denetimi: `phys` 3D fiziğinin ve ECS'in aynı girdilerle bit-bit aynı sonuç verdiğini testle doğrula;
  bozan yerler varsa (ör. sırasız iş paralelliği, NaN) Notlar'a yaz ve mümkünse düzelt.
- Girdi kaydı/oynatma (replay) — aynı kayıttan aynı hash dizisi.

## Dosyalar
- `src/sim/*` (yeni), `tests/test_sim.c` (yeni), `Makefile` (yalnız yeni kaynaklar gerekiyorsa)
- `src/phys/*`, `src/scene/*` (yalnız belirlenimcilik düzeltmeleri)
- `docs/DECISIONS.md` (D-110 kaydı), `docs/ARCHITECTURE.md` (modül listesi)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Test: iki "eş" simülasyon, biri girdileri 1–8 kare gecikmeli alıyor; rollback sonrası her karede hash'ler aynı
- [x] Test: 10 000 kare fizik + ECS replay iki kez → hash dizileri aynı
- [x] Ölçüm: 8 kare geri sarma + yeniden simülasyon maliyeti (örnek sahne) Notlar'da

## Notlar
- **Deterministik simülasyon ve rollback (`src/sim/sim.h`, `src/sim/sim.c`)**:
  - Sabit zaman adımlı (varsayılan `1/60 s`) simülasyon döngüsü (`sim_tick`, `sim_step`), en fazla 8 oyuncu ve 256 karelik oyuncu başına girdi halkası (`SIM_INPUT_RING_SIZE = 256`) eklendi.
  - Tahmini girdi modeli: gerçek girdi henüz ulaşmamışsa oyuncunun bilinen en son gerçek (veya tahmin edilmiş) girdisi tekrarlanır (`repeat-last-input`). Geçmiş bir kare için gecikmeli gerçek girdi `sim_submit_input` ile geldiğinde tahminle karşılaştırılır; uyuşmazlık varsa en eski hatalı tahmin karesi (`dirty_from_frame`) işaretlenir ve bir sonraki `sim_step` (veya açık `sim_rollback_to`) çağrısında o karenin giriş snapshot'ı geri yüklenip aradaki tüm kareler güncel girdilerle yeniden simüle edilir.
  - Durum anlık görüntüsü (snapshot) halkası: ikinin kuvveti kapasiteli (varsayılan 16 kare) dairesel tamponda her karenin simülasyon öncesi durumu düz `memcpy` ile saklanır. Oyun durumunun işaretçiden bağımsız kalması için `SimOffset`, `SIM_NULL_OFFSET`, `SIM_PTR`, `SIM_CONST_PTR` ve `sim_state_alloc` yardımcıları sağlandı ve belgelendi (`docs/DECISIONS.md` D-110).
  - Durum özeti ve senkron kaybı tespiti: FNV-1a + SplitMix64 bit karıştırıcılı 64-bit özetleyici (`sim_hash`, `sim_hash_update`, `sim_hash_final`), `-0.0f` ile `+0.0f` ve `NaN` bit temsillerini kanonikleştiren `sim_hash_f32`/`sim_hash_f32_slice`, `PhysWorld` ve `EcsWorld` içindeki ham işaretçileri atlayıp yalnızca deterministik durum verilerini özetleyen `sim_hash_phys` ve `sim_hash_ecs`, ayrıca eş kare özetlerini karşılaştıran `sim_check_desync` eklendi.
  - Replay kaydı ve ikili serileştirme: `SimReplay` (`sim_replay_init`, `sim_replay_record_step`, `sim_replay_step`, `sim_replay_save_bytes`, `sim_replay_load_bytes`) ile başlangıç tohumu, oyuncu sayısı ve kare başı girdi/özet akışı kaydedilip tekrar oynatılabiliyor.
- **Belirlenimcilik denetimi (`src/phys/*`, `src/scene/*`) bulguları ve düzeltmeleri**:
  1. `src/phys/shapes.h`: `PhysShape` içindeki `union as` alanı en büyük üye olan `Obb` nedeniyle 60 bayttır; `Sphere` ve `Plane` ise 16 bayt kullanır. `shape_from_sphere`, `shape_from_aabb`, `shape_from_capsule`, `shape_from_plane` yardımcıları yığında başlatılmamış `PhysShape` oluşturduğu için birliğin (union) kalan 32–44 baytında yığın çöpü kalıyor ve `phys_add_static` ile durum arenasına kopyalandığında ham bellek özetini (raw byte hash) bozuyordu. Tüm `shape_from_*` yardımcılarına `__builtin_memset(&sh, 0, sizeof(sh))` eklendi (`shape_from_obb` de eklendi).
  2. `src/scene/transform.c`: `quat_norm` içindeki `if (len2 <= 0.0f)` kontrolü `NaN` girdisinde `false` dönüp `NaN` yayılımına yol açabiliyordu; `if (!(len2 > 0.0f))` olarak düzeltildi. Ayrıca `transform_eval_entity` içinde `walk_stack` elemanları pop edilirken sıfırlandı.
  3. `src/scene/ecs.c`: `ecs_store_remove_idx` ile yoğun (dense) diziden silinen son slotun eski verisi, `ecs_spawn` içinde `free_list`'ten çekilen slot, `ecs_set` içindeki hizalama (stride) kuyruk baytları ve `ecs_flush` sonrası komut tamponu sıfırlanarak arena belleğinin deterministik kalması sağlandı.
- **Ölçümler (`sim_benchmark_8_frame_rollback` — örnek 3B fizik + ECS sahnesi)**:
  - Örnek sahne: 32 KB tekil durum arenası içinde `PhysWorld` (~500 üçgenlik BVH statik zemin + 4 duvar düzlemi + 2 oyuncu kapsülü + 6 dinamik küre + 8 mermi gövdesi) ve `EcsWorld` (`PawnComp`, `ProjectileComp`, `SpinnerComp`, `Transform` + ebeveyn-çocuk hiyerarşisi), 2 oyuncu.
  - 101 tekrar üzerinden medyan süreler (`test_runner`, `-O1 -fsanitize=address,undefined` altında):

| İşlem | Medyan Süre (ms) |
|---|---|
| Durum snapshot geri yükleme (`memcpy`, 32 KB) | `0.0013 ms` |
| 1 kare simülasyon adımı (`phys` + `ecs` + `transform` + `hash`) | `0.0318 ms` |
| 8 kare geri sarma + yeniden simülasyon (`sim_rollback_to`) | `0.2541 ms` |
| Tek kare durum özeti (`sim_state_hash`: `sim_hash_phys` + `sim_hash_ecs`) | `0.0036 ms` |

- **Ertelenenler**: Ağ soket/WebRTC/WebSocket taşıma katmanı görev tanımı gereği bu görev kapsamı dışındadır (sonraki ağ görevine bırakıldı).
