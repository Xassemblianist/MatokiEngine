# S1 — Ofset tabanlı durum bölgesi prototipi (D-112, ARCH_V2 §2/§6)

Durum: DONE (agy)

## Amaç
`src/state/` altında: taban adres saklamayan `RegionArena` (ofset döner), `StateRef<T>` benzeri tipli ofset/tutamaç
makroları, sürüm/nesil kontrollü tutamaçlar; durum başlıklarında (`state_*.h`) işaretçi alanı yasağı için `make check`
denetimi (betik + `_Static_assert`). Küçük kanıt: ECS benzeri SoA depo + fizik gövde dizisi dilimi işaretçisiz; anlık
görüntü = bölgenin kopyası; bölge **farklı taban adrese** kopyalanıp orada da çalışır ve hash aynı kalır (asıl kanıt).
Ölçüm: 16 MB kopya + hash süresi (F0_D ile karşılaştır).

## Dosyalar
- `src/state/*` (yeni), `tests/test_state.c` (yeni), `tools/host/check_state_headers.sh` (yeni), `Makefile` (yalnız denetim + kaynaklar)

## Kabul kriterleri
- [x] `make check` hatasız; işaretçi denetimi kasıtlı hatalı örnekte başarısız olur (test)
- [x] Farklı taban adreste aynı hash; ölçümler Notlar'da

## Notlar
- **Ofset tabanlı durum bölgesi ve `RegionArena` (`src/state/state_region.h`, `src/state/state_region.c`)**:
  - `RegionArena` (`cap`, `used`, `_pad0`, `_pad1` — 16 bayt) mutlak taban adresi (`void* base`) saklamaz ve hizalamayı mutlak adrese göre değil bölge başından olan `used` ofsetine göre hesaplar (`region_alloc_raw`, `region_alloc`). Böylece farklı taban adreslerde başlatılan iki durum bölgesinin iç yerleşimi bayt-bayt aynıdır.
  - `region_alloc` hizalama boşluğu (padding) dahil ayrılan aralığı sıfırlar; `region_arena_pop_to` geri alınan aralığı sıfırlayarak geçici ayırmaların ham bellek özetinde iz bırakmasını önler.
  - `StateRegionHeader` (64 bayt, ofset 0): `STATE_REGION_MAGIC` (`"MSTA"`), `version`, `abi_hash` (D-112 eşleşme kontrolü), `root_off`, `tick`, `flags` ve gömülü `RegionArena` taşır.
  - `STATE_REF_DEFINE(T)` ile her tip için derleme zamanı tip güvenli `StateRef(T)` (`{ StateOffset off; }`, 4 bayt) ve `StateSlice(T)` (`{ StateOffset off; u32 count; }`, 8 bayt) üretilir; `STATE_ALLOC`, `STATE_ALLOC_ARRAY`, `STATE_ALLOC_SLICE`, `STATE_DEREF`, `STATE_DEREF_CHECKED`, `STATE_AT` ve `STATE_SLICE_PTR` makroları sağlandı.
  - `StateHandle` (`[31..20: 12-bit nesil (1..4095)] [19..0: 20-bit yuva indeksi]`, `0 = STATE_HANDLE_NULL`), tipli `StateHandleOf(Tag)` sarmalayıcısı ve dizilerini `RegionArena` içinde ofsetlerle tutan işaretçisiz `StateSlotTable` gerçeklendi. Silinen yuvalarda nesil sayacı `1..4095` aralığında sarmalı artırılır (`0` atlanır) ve eski tutamaçlarla erişim (`use-after-free` / çift silme) reddedilir.
- **Durum başlıklarında (`state_*.h`) işaretçi alanı yasağı (`tools/host/check_state_headers.sh` + `_Static_assert`)**:
  - Derleme zamanında `STATE_ASSERT_NO_PTR_FIELD(T, field)` (`__builtin_classify_type != 5`) ve `STATE_ASSERT_VALUE_STRUCT(T, expected_bytes)` ile tüm durum yapılarının işaretçisiz ve sabit boyutlu (wasm32 ile 64-bit host arasında özdeş) olduğu doğrulanır.
  - `tools/host/check_state_headers.sh` betiği `src/**/state_*.h` dosyalarındaki tüm `struct`/`union` gövdelerini ve `typedef` bildirimlerini tarar; ham işaretçi (`*`), fonksiyon işaretçisi, platforma bağlı boyutlu tip (`usize`, `isize`, `uptr`, `size_t`, `uintptr_t`, `ptrdiff_t`, `long`) veya eksik `_Static_assert` tespit ettiğinde hata verir. `Makefile` içindeki `check` hedefine bağlandı.
  - `TEST(state_header_pointer_check_rejects_bad_header)` içinde `build/` altında oluşturulan 4 kasıtlı hatalı başlık örneğinde (ham işaretçi, fonksiyon işaretçisi, `usize` alanı, eksik `_Static_assert`) betiğin sıfır olmayan çıkış koduyla başarısız olduğu doğrulandı.
- **İşaretçisiz SoA ECS + fizik gövde dilimi ve farklı taban adreste çalışma kanıtı (`src/state/state_world.h`, `src/state/state_world.c`, `tests/test_state.c`)**:
  - `StateEcsSoA` (136 bayt) ve `StatePhysSlice` (104 bayt) tamamen `StateSlice(T)` ofset dilimleri ve `StateHandleOf(Entity)` / `StateHandleOf(PhysBody)` nesilli tutamaçlarıyla çalışır; kayan nokta çıktıları `state_canon_f32` (`-0.0f -> +0.0f`, NaN kanonikleştirme) ile temizlenir ve yoğun dizilerden silinen kuyruk yuvaları sıfırlanır.
  - `TEST(state_world_relocate_different_base_same_hash)`: `base_a` adresinde 80 adım koşulan dünya `state_region_relocate` (`memcpy`) ile farklı bir `base_b` adresine taşınır, eski `base_a` bölgesi `0xDD` ile zehirlenir ve simülasyon `base_b` adresinde 80..160 adımları arasında (dinamik spawn/destroy ve küre-küre/duvar çarpışmalarıyla) sürdürülür. Baştan sona üçüncü bir `base_c` adresinde koşan referans bölgeyle hem aktif bölge özeti (`state_region_hash_active`), hem tam bölge özeti (`state_region_hash_full`), hem de `memcmp` bit-bit aynıdır.
- **16 MB durum bölgesi kopya + hash ölçümleri (F0_D / H4 karşılaştırması)**:
  - Ortam: `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz`, Linux, 10 ısınma + 41 tekrar medyanı. Paylaşımlı makine yükü (`uptime`): `load average: 4.81, 5.01, 4.84`.
  - `state_region_hash_bytes` içinde 32-baytlık 4 bağımsız biriktiricili (`v1..v4`, xxHash64 benzeri) özet döngüsü gerçeklendi ve M16_A'daki tek hatlı `sim_hash` ile karşılaştırıldı:

| İşlem | Host `-O1` (ASan+UBSan, `test_runner`) | Host `-O2` (üretim/wasm eşdeğeri) | Not / H4 (`< 2 ms`) Karşılaştırması |
|---|---:|---:|---|
| **16 MB kopya** (`state_region_snapshot_save` / `memcpy`) | `1.478 – 2.302 ms` (`6.8 – 10.6 GB/s`) | **`1.739 ms`** (`9.0 GB/s`) | H4 `< 2 ms` bütçesi içinde |
| **16 MB 4-hatlı hash** (`state_region_hash_full`) | `3.593 ms` (`4.3 GB/s`) | **`1.532 ms`** (`10.2 GB/s`) | Tek hatlı `sim_hash`'e göre **2.35× hızlı** (`-O2`) |
| **16 MB 1-hatlı hash** (`sim_hash`, M16_A referansı) | `4.534 – 5.117 ms` (`3.1 – 3.4 GB/s`) | `3.600 ms` (`4.3 GB/s`) | Seri 8-bayt bağımlılık zinciri |
| **16 MB kopya + 4-hatlı hash toplamı** | `6.040 – 6.173 ms` | **`3.439 ms`** | Tam 16 MB her kare kopya+hash ağır |
| **Aktif dünya önyüzü (`173 KB`, `used` bayt) kopya** | `0.0121 – 0.0182 ms` | **`0.0150 ms`** (`15.0 µs`) | 16 MB yerine `arena.used` kopyalamak **~115× hızlı** |
| **Aktif dünya önyüzü (`173 KB`, `used` bayt) hash** | `0.0280 – 0.0440 ms` | **`0.0115 ms`** (`11.5 µs`) | `state_region_hash_active` kare başına ihmal edilebilir |

  - **Sonuç (F0_D / H4 değerlendirmesi)**: 16 MB ham `memcpy` tek başına `-O2`'de `1.739 ms` ile 2 ms sınırının altındadır; ancak 60 Hz'de her kare 16 MB kopya + 16 MB hash (`3.44 ms/kare`) kare bütçesinin %20'sini tüketir. `RegionArena`'nın `used` sınırını (`state_region_hash_active` ve yalnız `[0..used)` dilimini snapshot'lamak) kullanmak tipik simülasyon durumunda (`173 KB`) kopya+hash toplamını **`0.0265 ms`'ye (`26.5 µs`)** indirir.
