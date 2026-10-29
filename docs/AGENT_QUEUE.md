# Özerk ajan kuyruğu (Argon + Flash, ikisi de high)

Yönetici olmadan çalışır: her ajan kendi dalında işi bitirir, `merge.sh` ile **kendisi** main'e alır
(kilit altında merge → main'de `make check` → GitHub'a push). Kırılırsa merge geri alınır, ajan düzeltip tekrar dener.

## Başlatma
```bash
nohup /home/ostriquetrum/matoki-wt/queue/run2.sh > /home/ostriquetrum/matoki-wt/logs/run2.log 2>&1 &
```
- **2026-10-29:** agy haftalık limiti %9 kaldığı için yalnız R0 + G1_A başlatıldı; tam liste `items2_full.txt` (limit yenilenince `cp items2_full.txt items2.txt` ve yeniden başlat).
- Aynı anda en çok 4 ajan (`MAX=7 run2.sh` → 6 ajan). Bağımlılığı olan görev, bağımlı olduğu görev main'de DONE olunca başlar.
- İzleme: `tail -f /home/ostriquetrum/matoki-wt/logs/run2.log`, ajan logları `logs/<görev>.log` (sonunda `BITTI <görev> MERGED` ya da `MERGE_EDILMEDI`).
- Sonuçlar: `make status`, görev dosyalarının "Notlar" bölümü, `git log main`.
- Durdurmak: `pkill -f run2.sh` (çalışan ajanlar bitene kadar sürer).
- Z1 bittikten sonra sonraki dalga: `items3.txt`'yi `items2.txt` yerine koyup yeniden başlat.

## Model ataması
| Görev | Model | Bağımlılık | Ne |
|---|---|---|---|
| [R0_resolution](../tasks/R0_resolution.md) | 3.8-flash-high | — | R0 — Çözünürlük seçimi (hızlı kazanç) |
| [G1_A_gpu_ocean](../tasks/G1_A_gpu_ocean.md) | 4-argon-high | — | G1_A — Okyanus WebGPU'da tam çözünürlükte (ilk GPU sahnesi) |
| [P1_A_phys2_switch](../tasks/P1_A_phys2_switch.md) | 3.8-flash-high | — | P1_A — phys → phys2 geçişi |
| [T1_matoki_fuzz](../tasks/T1_matoki_fuzz.md) | 3.8-flash-high | — | T1 — Matoki diferansiyel fuzz testi (ir_eval kahin) |
| [V1_sky_clouds](../tasks/V1_sky_clouds.md) | 4-argon-high | G1_A_gpu_ocean | V1 — Fiziksel atmosfer + hacimsel bulutlar + gün döngüsü (GPU) |
| [G1_B_gpu_scene](../tasks/G1_B_gpu_scene.md) | 4-argon-high | G1_A_gpu_ocean | G1_B — GPU'da yaşayan sahne + gfx3d WebGPU arka ucu (H5) |
| [P1_B_solver_parallel](../tasks/P1_B_solver_parallel.md) | 4-argon-high | P1_A_phys2_switch | P1_B — Rijit çözücü paralel ölçekleme |
| [P1_E_phys_bench](../tasks/P1_E_phys_bench.md) | 3.8-flash-high | P1_A_phys2_switch | P1_E — Fizik benchmark + regresyon paketi |
| [T2_gpu_cpu_compare](../tasks/T2_gpu_cpu_compare.md) | 3.8-flash-high | G1_A_gpu_ocean | T2 — CPU ↔ GPU karşılaştırma testi |
| [V2_terrain](../tasks/V2_terrain.md) | 4-argon-high | G1_B_gpu_scene | V2 — Prosedürel ada: arazi + bitki örtüsü (GPU, yeni sahne "10:Ada") |
| [P1_F_gpu_fluid](../tasks/P1_F_gpu_fluid.md) | 4-argon-high | G1_B_gpu_scene | P1_F — GPU akışkan / parçacık fiziği (PBF) |
| [Z1_next_wave](../tasks/Z1_next_wave.md) | 4-argon-high | V1_sky_clouds,P1_B_solver_parallel,V2_terrain,P1_F_gpu_fluid | Z1 — Sonraki dalganın planı (yalnız belge) |

Neden böyle: **Argon** görsel/3D/karmaşık sistemlerde güçlü (okyanus M17_A tek seferde fiziksel doğru çıktı) ama
inceleme/sadakat işinde uyduruyor → ona sahne, shader, GPU, çözücü işleri. **Flash** dar, mekanik, test ağırlıklı işlerde
hızlı ve güvenilir → çözünürlük, phys geçişi, fuzz, karşılaştırma, benchmark.

## Her ajana giden prompt
`__T__` = görev adı, `__M__` = model (betik doldurur).

```text
Sen MATOKI projesinde (WebGPU + WebAssembly oyun motoru) tam yetkili bir ajansın; yönetici yok, işi baştan sona sen bitirip main'e alacaksın.
Çalışma dizinin bu git worktree'si (dal: __T__). Önce AGENTS.md, docs/ARCHITECTURE.md, docs/ARCH_V2.md, docs/DECISIONS.md ve görev dosyan tasks/__T__.md'yi oku (Matoki derleyicisine dokunuyorsan docs/MATOKI.md de).
Kurallar:
- Görev dosyasındaki Durum'u 'IN_PROGRESS (agy __M__)' yap. Yalnızca 'Dosyalar' bölümündeki dosyalara dokun; zorunlu ek dosya gerekirse dokun ve Notlar'a gerekçesini yaz.
- src/ altında libc include yok, malloc yok, mevcut stil (Türkçe yorumlar, modul_fonksiyon, dosya başı açıklama). Uyarıyı pragma ile susturma. Testi zayıflatma/silme.
- Sık sık 'make check' çalıştır; hatasız ve uyarısız geçmeli. Geçici dosyalar worktree içindeki build/ dizinine (/tmp değil).
- Gerçek GPU/tarayıcı doğrulaması: 'make gpucheck' ve tools/host/webgpu_check.mjs (headless Chrome + Vulkan, RTX 2060). Ekran görüntüsünü build/ altına al ve gerçekten bak.
- Optimizasyon iddiası = ölçüm. Makine başka ajanlarla paylaşılıyor: ölçümde 'uptime' yükünü not et. Uydurma yok; gözlemediğini yazma.
- Mimari kararı değişiyorsa docs/DECISIONS.md'ye yeni D-xxx (mevcut son numaradan devam) ekle.
- Soru sorma; belirsizlikte en basit doğru seçimi yap, Notlar'a yaz.
Bitiş (zorunlu, sırayla):
1. Kabul kriterlerini doğrula; Durum'u DONE yap, kısa 'Notlar' (yapılan, ertelenen, ölçümler) ekle.
2. Türkçe mesajla bu dala commit et (sonuna boş satır + 'Co-Authored-By: Gemini <noreply@google.com>').
3. 'git merge main' yap (çakışma varsa iki tarafı da koruyarak çöz), 'make check' geçsin, commit et.
4. '/home/ostriquetrum/matoki-wt/queue/merge.sh __T__' çalıştır. 'MERGED' görene kadar: çıkış 2 ise 3. adıma dön; 3 ise logdaki hatayı düzelt, 3. adıma dön; 4 ise 2 dk bekleyip tekrar dene (en çok 10 kez). main'e elle merge/push/reset YAPMA, yalnız bu betik.
5. Başaramazsan Durum'u 'BLOCKED: <neden>' yapıp dalına commit et. Son çıktın: kısa Türkçe özet.
```

## Betikler (`/home/ostriquetrum/matoki-wt/queue/`)

### run2.sh
```bash
#!/bin/bash
# Özerk kuyruk: items2.txt satırları "görev model [bağımlılık,...]". Bağımlılıklar main'de DONE olunca başlar.
# Her ajan kendi worktree'sinde çalışır ve merge.sh ile kendisi main'e alır + yayınlar. Aynı anda en çok MAX ajan.
# agy hub süreci de sayılır: MAX=5 → 4 ajan
MAX=${MAX:-5}; W=/home/ostriquetrum/matoki-wt; MAIN=/home/ostriquetrum/WEBASSEMBLY; cd $W
done_in_main() { grep -q '^Durum: DONE' "$MAIN/tasks/$1.md" 2>/dev/null; }
while read -r t m d; do
  [ -z "$t" ] || [[ $t == \#* ]] && continue
  done_in_main "$t" && { echo "atla $t (DONE)"; continue; }
  for x in ${d//,/ }; do until done_in_main "$x"; do sleep 60; done; done
  while [ "$(pgrep -x agy | wc -l)" -ge "$MAX" ]; do sleep 30; done
  git -C $MAIN worktree add -q -b "$t" "$W/$t" main 2>/dev/null || git -C $MAIN worktree add -q "$W/$t" "$t"
  P=$(sed "s/__T__/$t/g; s/__M__/$m/g" queue/prompt.txt)
  ( cd $W/$t && agy -p "$P" --model "$m" --dangerously-skip-permissions > $W/logs/$t.log 2>&1
    if git -C $MAIN merge-base --is-ancestor "$t" main; then git -C $MAIN worktree remove --force "$W/$t"; git -C $MAIN branch -D "$t"; echo "BITTI $t MERGED" >> $W/logs/$t.log
    else echo "BITTI $t MERGE_EDILMEDI" >> $W/logs/$t.log; fi ) &
  echo "$(date +%H:%M) basladi $t $m"; sleep 10
done < queue/items2.txt
wait; echo "KUYRUK BITTI $(date)"
```

### merge.sh
```bash
#!/bin/bash
# Ajan kendi dalını main'e alır: kilit altında merge + make check + GitHub yayını. Kullanım: merge.sh <dal>
# Çıkış: 0 alındı, 2 çakışma (önce worktree'de 'git merge main'), 3 main'de make check kırıldı (geri alındı), 4 main kirli.
set -u
B=$1; MAIN=/home/ostriquetrum/WEBASSEMBLY; Q=/home/ostriquetrum/matoki-wt/queue; L=/home/ostriquetrum/matoki-wt/logs
exec 9>$Q/merge.lock; flock 9
cd $MAIN
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "main kirli; dokunma, logs/'a not düş"; exit 4; }
git merge --no-ff -q "$B" -m "Merge $B" || { git merge --abort; echo "ÇAKIŞMA: worktree'nde 'git merge main' yap, çöz, make check, tekrar dene"; exit 2; }
if ! make check > $L/check_$B.log 2>&1; then git reset -q --hard ORIG_HEAD; echo "main'de make check kırıldı, merge geri alındı: $L/check_$B.log"; exit 3; fi
$Q/publish.sh > $L/publish_$B.log 2>&1 || echo "UYARI: push başarısız (merge yerinde): $L/publish_$B.log"
echo "MERGED $B"
```

### items2.txt
```bash
# görev model bağımlılık
R0_resolution gemini-3.8-flash-high
G1_A_gpu_ocean gemini-4-argon-high
P1_A_phys2_switch gemini-3.8-flash-high
T1_matoki_fuzz gemini-3.8-flash-high
V1_sky_clouds gemini-4-argon-high G1_A_gpu_ocean
G1_B_gpu_scene gemini-4-argon-high G1_A_gpu_ocean
P1_B_solver_parallel gemini-4-argon-high P1_A_phys2_switch
P1_E_phys_bench gemini-3.8-flash-high P1_A_phys2_switch
T2_gpu_cpu_compare gemini-3.8-flash-high G1_A_gpu_ocean
V2_terrain gemini-4-argon-high G1_B_gpu_scene
P1_F_gpu_fluid gemini-4-argon-high G1_B_gpu_scene
Z1_next_wave gemini-4-argon-high V1_sky_clouds,P1_B_solver_parallel,V2_terrain,P1_F_gpu_fluid
```
