#!/bin/bash
# Yarıda kalan (MERGE_EDILMEDI) görevi korunmuş worktree'sinde sürdürür. Kullanım: resume.sh <görev> <model>
t=$1; m=$2; W=/home/ostriquetrum/matoki-wt; MAIN=/home/ostriquetrum/WEBASSEMBLY; cd $W/$t || exit 1
BASE=$(git -C $MAIN rev-parse main)
P="$(sed "s/__T__/$t/g; s/__M__/$m/g" $W/queue/prompt.txt)
ÖNEMLİ: Bu görev daha önce yarıda kaldı; worktree'deki commit edilmemiş değişiklikler önceki ajanın işi. Önce 'git status' + 'git diff --stat' ile incele, baştan yazma, kaldığı yerden bitir."
agy -p "$P" --model "$m" --dangerously-skip-permissions < /dev/null >> $W/logs/$t.log 2>&1
if git -C $MAIN merge-base --is-ancestor "$t" main && [ "$(git -C $MAIN rev-list --count $BASE..$t)" != 0 -o "$(git -C $MAIN rev-parse $t)" = "$(git -C $MAIN rev-parse main)" ] && grep -qi '^durum: *done' $MAIN/tasks/$t.md; then
  git -C $MAIN worktree remove --force "$W/$t"; git -C $MAIN branch -D "$t"; echo "BITTI $t MERGED" >> $W/logs/$t.log
else echo "BITTI $t MERGE_EDILMEDI (worktree korundu: $W/$t)" >> $W/logs/$t.log; fi
