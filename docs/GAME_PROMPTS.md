# Oyun ve mekanik promptları

Motorun güçlü yanlarını sergileyen **özgün** oyunlar: belirlenimci rollback, gerçek okyanus, fizik, GPU akışkan,
Matoki, anında yükleme. Var olan bir oyunun kopyası/yeniden yapımı **yok**; türler ve mekanikler serbest.

## Kullanım
- **Tek ajan:** "Ortak başlık" + bir oyun promptu birlikte verilir (Argon high önerilir; "Flash" yazanlar dar işler).
- **Kuyruk:** Promptu `tasks/<ID>.md` olarak kaydet (Amaç = prompt, Dosyalar, Kabul kriterleri), `items2.txt`'ye satır ekle.
- **Sıra önerisi:** Önce **G0** (oyun çatısı); diğerleri onun üstüne.

---

## Ortak başlık (her promptun başına)

```text
MATOKI motorunda (WebGPU + wasm, C11 freestanding) özgün bir oyun yapıyorsun. Önce AGENTS.md, docs/ARCHITECTURE.md,
docs/ARCH_V2.md, docs/DECISIONS.md oku. Kurallar: src/ altında libc/malloc yok, Arena, Türkçe yorum, make check temiz.
Oyun mantığı sim-kritik yolda belirlenimci (D-112: tamsayı tick, relaxed SIMD/GPU yok, 1T=8T hash aynı); görsel
taraf GPU/relaxed serbest. Var olan ticari bir oyunu kopyalama; mekanik ve görsel kimlik özgün olsun.
Tasarım belgesini önce yaz (docs/games/<ad>.md: fantezi, çekirdek döngü 30 sn / 5 dk / 1 saat, kontroller,
ilerleme, zorluk eğrisi, "neden bu motorda" bölümü), sonra kodla. Oynanabilir dikey dilim hedefle: 1 seviye/harita,
ses, menü, kazanma/kaybetme, HUD. Hissi ölç: girdi gecikmesi (ms), kare süresi 1080p, ve oyun hissi için
coyote time / input buffer / hitstop / ekran sarsıntısı gibi "juice" ayarlarını sabitler olarak aç.
Headless Chrome (tools/host/webgpu_check.mjs) ile ekran görüntüleri al, gerçekten bak. Bitince görev Notlar'ı +
merge.sh (docs/AGENT_QUEUE.md protokolü).
```

---

## G0 — Oyun çatısı (Flash high, önce bu)

```text
Oyunlar için ortak altyapı (src/game/): sahne yığını (menü → oyun → duraklat → sonuç), belirlenimci girdi eşleme
(klavye/gamepad/dokunmatik → tick başına bit maskesi, src/sim girdi halkasına), kamera yardımcıları (takip, sarsıntı,
yumuşatma; görsel sınıf), olay/zamanlayıcı sistemi (tick tabanlı), kayıt/yükleme (OPFS, try/catch), ses tetikleyici
API'si, HUD/menü widget'ları (src/ui üstüne), "juice" yardımcıları (hitstop, tween, easing, parçacık patlaması),
replay kaydı + oynatma (girdi akışı → bit-bit aynı). Örnek: 1 dakikalık mini oyun ile hepsini kullan.
Kabul: replay 3 kez oynatılınca aynı hash; gamepad + dokunmatik çalışıyor; make check temiz.
```

---

## 1. "Gelgit" — okyanusta yelkenli yarış + savaş (Argon)

```text
Okyanus sahnesinin (M17_A/G1_A FFT dalgaları) üstüne fiziksel yelkenli oyunu. Mekanikler: rüzgâr yönü ve şiddeti
değişken (cepheler, rüzgâr kayması), yelken açısı + dümen ile gerçek "tacking" (rüzgâra karşı zikzak), dalga
sörfü (dalga yüzünden aşağı inerken hız kazanma), alabora riski (yalpa açısı eşiği). Modlar: (a) şamandıralı
yarış parkuru, hayalet rakip = kendi replay'in; (b) 2–8 oyunculu rollback çok oyunculu yarış; (c) zıpkın/halat
ile rakibi yavaşlatma, çarpışmada hasar. Fizik belirlenimci CPU tarafında (kaldırma: dalga yüksekliği CPU FFT'den),
görsel köpük/sıçrama GPU'da. Kamera: geminin arkasında, dalgaya göre yumuşatılmış. Fırtına seviyesi: Hs 6 m+.
"Neden bu motorda": gerçek dalga fiziği ile oynanış (dalgayı okumak bir beceri).
```

## 2. "Kum Saati Düellosu" — rollback dövüş oyunu (Argon)

```text
1v1 yakın dövüş, rollback netcode'un vitrini. Özgün mekanik: her oyuncunun "zaman kumu" barı var; bar harcanarak
kendi karakterinin son 30 karesini geri sarabilir (rakip geri sarılmaz) → hatalı hamleyi iptal etme vs. kumu saldırıya
harcama ikilemi. Kare verisi tablolu (startup/active/recovery, hitbox/hurtbox tick tabanlı), girdi tamponu, iptal
(cancel) zincirleri, savunma (blok, kaçınma). 2 karakter, farklı arketip (yakın/menzilli). Ağ: WebRTC DataChannel,
girdi gecikmesi 1–2 kare + 8 kareye kadar rollback, desync algılama (her 60 tick'te hash). Yerel 2 oyunculu da olsun.
Ölç: 8 kare rollback süresi (< 1 ms hedef), 150 ms yapay gecikmede oynanabilirlik.
```

## 3. "Kum ve Su" — GPU akışkanlı bulmaca (Argon, P1_F sonrası)

```text
Akışkan (PBF, GPU) ve granüler malzeme (DEM, src/phys dem) ile bulmaca: oyuncu yerçekimini döndürür, set/boru
çizer, ısıtır/soğutur (su ↔ buz ↔ buhar faz değişimi); hedef: sıvıyı kaplara doğru oranlarda doldurmak, değirmen
çarkını döndürüp kapı açmak. 20 seviye, her biri tek mekaniği öğretir, sonra birleşim. Belirlenimcilik: akışkan görsel
sınıf olduğundan seviye çözümü "kapta ≥ %X sıvı" eşikli; replay için kritik değil. Ekran-uzayı akışkan çizimi + kırılma.
"Neden bu motorda": 256k parçacık tarayıcıda 60 fps.
```

## 4. "Yankı Labirenti" — sesle görme (Flash)

```text
Karanlık 3D labirent: oyuncu hiçbir şey görmez; ses dalgası (tık, adım, atılan taş) GPU'da ekran-uzayı "ses
yayılımı" efektiyle duvarları kısa süre görünür kılar. Ses yayılımı = ızgara üzerinde dalga denklemi (Matoki compute),
yankılar gerçek geometriden. Düşmanlar sese gider → gürültü yapmak hem görmek hem tehlike. 3D ses (HRTF) önemli.
Prosedürel labirent (tohumlu, belirlenimci), 5 kat, son katta kaçış.
```

## 5. "Kule Ustası" — fiziksel inşa + yıkım (Argon)

```text
İki taraf sırayla fiziksel bloklardan (ağaç, taş, metal; kütle/sürtünme farklı) kale kurar; sonra mancınık/top
ile karşılıklı yıkım. Yıkım: Voronoi kırılma (önceden kesilmiş parçalar), yapısal stres (eklem kopma eşiği), toz
parçacıkları GPU'da. Belirlenimci rijit fizik (P1_B paralel çözücü) sayesinde çevrimiçi: sadece girdiler gönderilir,
her iki tarafta bit-bit aynı yıkım. 10k parçaya kadar. Mod: tek oyunculu bulmaca ("bu kaleyi 3 atışta yık").
```

## 6. "Rüzgâr Gülü" — süzülme/keşif (Argon, V1+V2 sonrası)

```text
Prosedürel ada (V2) ve hacimsel bulutlar (V1) üstünde planör/kanat kostümü ile süzülme. Termikler (sıcak
yamaçlardan yükselen hava, gün saatine bağlı), rüzgâr yamaç kaldırması, bulut altı emme; enerji yönetimi = irtifa.
Keşif hedefleri: haritada gizli 30 "fener", her biri yeni rota açar. Gün-gece döngüsü oynanışı değiştirir (gece termik
yok). Kamera hız hissi: FOV + hareket bulanıklığı. "Neden bu motorda": web'de büyük açık dünya + atmosfer.
```

## 7. "Tick" — zaman döngüsü suç çözme (Flash + Argon)

```text
Küçük bir şehir meydanında 3 dakikalık zaman döngüsü; NPC'ler belirlenimci programlara göre yaşar (sim çekirdeği).
Oyuncu her döngüde gözlemler, not alır, olaylara müdahale eder; döngü sonunda dünya sıfırlanır (snapshot geri yükleme,
src/sim). Oyuncu bilgisi kalıcı. Hedef: bir olayı (ör. köprü çöküşü) önlemek için zincirleme müdahale. Editör:
zaman çizelgesinde ileri/geri sarma (rollback altyapısı) — oyuncuya "kaset" olarak verilir. Belirlenimciliğin
oynanışın kendisi olduğu bir oyun.
```

## 8. "Sürü" — 100k birimlik RTS (Argon, G1_B sonrası)

```text
Gerçek zamanlı strateji: her oyuncu tek tek askerler yerine akış alanlarıyla (flow field) yönlendirilen 50k birimlik
sürüler yönetir. Birimler sistem çekirdeklerinde (Matoki "system", CPU JIT SIMD + çok iş parçacığı) — belirlenimci,
böylece lockstep çok oyunculu sadece komut gönderir. Kaynak: arazideki enerji kristalleri; sürü bölme/birleştirme,
formasyon (yoğunluk alanı), birim türleri taş-kâğıt-makas. Çizim: GPU örnekleme, kırpma, LOD (H5 vitrini).
Ölç: 100k birim sim ms (8T), CPU gönderim ms.
```

## 9. "Ateş Hattı" — itfaiye simülasyonu (Argon)

```text
Yangın yayılımı hücresel otomat + rüzgâr + yakıt türü (çim, ağaç, bina), duman hacimsel (GPU), su akışkanı (P1_F)
ve köpük. Oyuncu ekip yönetir: hortum basıncı, su tankı, yangın şeridi kesme, helikopter su bırakma. Hedef: köyü
korumak. Isı haritası görsel; sim belirlenimci (replay ve çevrimiçi ortak oyun için).
```

## 10. "Ritim Mekaniği" — müzikle senkron platform (Flash)

```text
2.5D platform: dünya müziğin vuruşuna göre değişir (platformlar vuruşta belirir, düşman saldırıları ritimde).
Ses: AudioWorklet zamanı ↔ sim tick eşlemesi (gecikme kalibrasyon ekranı). Girdi değerlendirmesi: vuruşa ±50 ms
"mükemmel" pencere, combo çarpanı. Müzik prosedürel (src/audio sentez): seviye müziği tohumdan üretilir, seviye
müzikten üretilir. Coyote time + input buffer şart.
```

## 11. "Gemi Mezarlığı" — sualtı keşif + hayatta kalma (Argon)

```text
Okyanusun altı: ışık derinlikle söner (hacimsel su saçılması, caustics GPU'da), oksijen ve basınç yönetimi, batık
gemilerde (fiziksel kapılar, yüzen enkaz) eşya toplama. Yüzeyde dalgalar M17 okyanusu; dalış botu kaldırma fiziği.
Akıntılar (akış alanı) yüzmeyi etkiler. Yaratıklar: prosedürel animasyonlu (IK) — tehdit değil, ortam.
```

## 12. "Domino Laboratuvarı" — Rube Goldberg kum havuzu (Flash)

```text
Fizik kum havuzu: domino, rampa, makara, ip (XPBD), menteşe, yay, top, su (opsiyonel GPU); oyuncu makine kurar,
"Başlat" der. Belirlenimci olduğu için makine kodu = kısa paylaşım dizgisi (URL hash'te değil — kopyala/yapıştır metin),
başkasında bit-bit aynı çalışır. Görevler: "zili 5 sn içinde çal", "en çok parça zinciri". Geri al/yinele, zaman
yavaşlatma, kare kare ilerleme.
```

## 13. "Kar Fırtınası" — araç sürüşü + deformasyonlu kar (Argon)

```text
Raycast araç (src/phys vehicle) + deforme olan kar/çamur (yükseklik alanı GPU compute'ta tekerlek izleriyle güncellenir,
sim tarafında kaba ızgara ile direnç). Görev: dağ köyüne erzak taşımak; vinç ile sıkışan aracı çekme, lastik zinciri,
yakıt. Hava: kar parçacıkları, görüş mesafesi, gece farları (kümelenmiş ışık). Hissi sürüş: süspansiyon, yük kayması.
```

## 14. "Yapay Ekosistem" — yaşam simülasyonu (Flash + Argon)

```text
Ada üstünde evrimleşen canlılar: genom (hız, boyut, görüş, renk) → davranış (basit sinir ağı, Matoki system çekirdeği,
SIMD). Besin zinciri, mevsimler, mutasyon. Oyuncu tanrı rolünde: iklim, besin, felaket. 10k canlı, belirlenimci tohumlu;
grafikler: popülasyon, gen dağılımı. "Tohum paylaş" ile başkası aynı tarihi izler.
```

## 15. "Kale Kuşatması Online" — 16 oyunculu fiziksel takım oyunu (Argon, en son)

```text
2 takım × 8 oyuncu: biri kale savunur, biri kuşatma makineleri (koçbaşı, kule, mancınık) kurar ve iter. Tüm fizik
belirlenimci rollback ile senkron (sadece girdi gönderilir) — 16 oyunculu rollback'in sınırlarını ölç (CPU bütçesi:
geri sarma × oyuncu). Gerekirse hibrit: yakın etkileşim rollback, uzak nesneler yetkili sunucu (Node headless wasm).
Ölç: 16 oyuncu, 150 ms gecikme, desync oranı.
```

---

## Mekanik prototip promptları (küçük, Flash high)

Her biri 1 sahne, 1 mekanik. Amaç: hissi bulmak, ölçmek, sonra oyunlara taşımak.

| ID | Prompt |
|---|---|
| M1 | **Kanca + sallanma:** ipli kanca (XPBD ip), sallanırken momentum koruma, bırakma anında hız bonusu; ayarlanabilir ip boyu. |
| M2 | **Duvar koşusu + kayma:** karakter kontrolcüsüne duvar koşusu, eğimde kayma, kayarken zıplamayla hız aktarımı. |
| M3 | **Yerçekimi alanları:** küresel/silindirik yerel yerçekimi, karakter yönelimi yumuşak dönsün; küçük gezegenler arası zıplama. |
| M4 | **Portal yok, "kat" var:** dünyayı katlayan düzlem — iki uzak noktayı birleştirir (görsel: render-to-texture, fizik: ışın geçişi). |
| M5 | **Mıknatıs:** iki kutup, metal nesneleri çekme/itme, kendini fırlatma; kuvvet alanı görselleştirme. |
| M6 | **Zaman yavaşlatma balonu:** bölgesel zaman ölçeği (sim'de nesne başına tick bölücü, belirlenimci). |
| M7 | **Boya ile yüzey özelliği:** boyanan yüzey sürtünmesiz/zıplatan/yapışkan olur (GPU boya dokusu + sim tarafı kaba ızgara). |
| M8 | **Yıkılabilir arazi:** voksel/SDF kazı, düşen parçalar rijit cisme dönüşür. |
| M9 | **Kalabalık kaçışı:** 10k NPC panik akışı (akış alanı + kaçınma), oyuncu yol açar/kapatır. |
| M10 | **Sörf:** okyanus dalga yüzünde tahta fiziği, dalga kırılma bölgesi, tüp içi kamera. |

## Kabul kriterleri (hepsi için)
- [ ] `docs/games/<ad>.md` tasarım belgesi
- [ ] Oynanabilir dikey dilim, 1080p ekran görüntüleri `build/` altında
- [ ] Replay: aynı girdi → aynı hash (sim-kritik oyunlarda)
- [ ] Girdi gecikmesi ve kare süresi ölçümü Notlar'da
- [ ] `make check` (+ gerekiyorsa `make gpucheck`, `make detcheck`) temiz
