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
