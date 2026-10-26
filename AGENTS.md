# Ajan Kuralları (MATOKI)

Bu repoda çalışan her AI ajanı önce bunu, sonra `docs/ARCHITECTURE.md`'yi okur.
Matoki derleyicisine dokunan görevlerde `docs/MATOKI.md` de okunur.

## İş akışı
0. Repo git altında: her görev kendi branch'inde (`git checkout -b <görev-adı>`), bitince `main`'e merge.
1. `tasks/` altındaki **en küçük numaralı, durumu `TODO` olan** görevi al (`make status` hepsini listeler).
2. Görev dosyasının başındaki durumu `IN_PROGRESS (ajan adı)` yap.
3. Sadece görevin "Dosyalar" bölümündeki dosyalara dokun. Başka dosya gerekiyorsa görev dosyasına not düş.
4. `make check` (= `make` + `make test` + kural denetimi) hatasız + uyarısız geçmeli. Görevdeki "Kabul kriterleri"nin hepsini doğrula.
5. Durumu `DONE` yap, altına kısa bir "Notlar" bölümü ekle (ne yaptın, neyi ertelediğin, benchmark sonuçları).

## Kod kuralları
- C11, `-Wall -Wextra -Werror`. `src/` altında libc başlığı include etmek YASAK (`<stdint.h>` dahil — `core/types.h` kullan).
  İstisna: `tools/host/` native araçları libc kullanabilir.
- Tipler: `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 b32 usize`.
- İsimlendirme: `modul_fonksiyon()` (ör. `raster_triangle`), tipler `PascalCase`, sabitler `UPPER_SNAKE`.
- malloc yok. Bellek her zaman bir `Arena*` veya pool'dan.
- Her modülün başında 3-5 satırlık açıklama yorumu.
- Optimizasyon iddiası = benchmark sonucu. Ölçmeden "hızlandırdım" yazma.
- Mimari kararı değiştiren bir şey gerekirse **kodlama**, `docs/DECISIONS.md`'ye öneri olarak yaz. Alınmış kararlar da orada (önce oku).

## JS kuralları
- Saf ES modülleri, bağımlılık/npm YOK, bundler YOK.
- Motor mantığı JS'e giremez. JS yalnızca `ARCHITECTURE.md §2`'deki sözleşmeyi uygular.
