# F0_F — ARCH_V2 kırmızı takım incelemesi — yalnız inceleme

Durum: DONE

## Amaç
`docs/ARCH_V2.md`'yi sert bir kıdemli motor mimarı gibi eleştir: yanlış varsayımlar, web platformunda imkânsız ya da
çok pahalı olanlar, eksik katman/kararlar, Unreal/Frostbite/id Tech/Bevy mimarileriyle kıyas, hipotezlerin ölçütleri
yeterli mi, riskler ve sıra. Kod tabanını (yalnız oku) ARCH_V2 ile kıyasla: neyi yeniden yazmak gerekir.
Somut, maddeli, öncelik sıralı öneriler.

## Dosyalar
- `docs/ARCH_V2_REVIEW.md` (yeni; tek dosya). Başka dosyaya yazma.

## Kabul kriterleri
- [x] Her eleştiri ARCH_V2'deki bir bölüme bağlı ve önerili
- [x] "En yüksek riskli 5 karar" ve "ilk yapılması gerekenler" bölümleri

## Notlar
- Çıktı: `docs/ARCH_V2_REVIEW.md` (tek dosya). Yalnız okuma; koda dokunulmadı, `make check` gerekmedi (belge değişikliği).
- Her koda dair iddia `dosya:satır` ile bağlı; doğrulanamayan web iddiaları `⚠ doğrulanmalı` ve §10'da listeli (F0_E girdisi).
- Ana bulgular: belirlenimcilik ↔ D-109/D-036 çelişkisi, işaretçili durum, IR yeniden tasarımı, JIT yaşam döngüsü, fizik kapsamı, faz sırası.
