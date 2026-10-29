#!/bin/bash
# Özerk kuyruk: items2.txt satırları "görev model [bağımlılık,...]". Her turda hazır olan (bağımlılıkları main'de DONE,
# worktree'si yok) tüm görevleri boş yer oldukça başlatır. Her ajan merge.sh ile kendisi main'e alır + yayınlar.
# agy hub süreci de sayılır: MAX=10 → 9 ajan. Commit atmadan biten ajanın worktree'si korunur (yeniden başlatılmaz).
MAX=${MAX:-10}; W=/home/ostriquetrum/matoki-wt; MAIN=/home/ostriquetrum/WEBASSEMBLY; cd $W
done_in_main() { grep -qi '^durum: *done' "$MAIN/tasks/$1.md" 2>/dev/null; }
launch() { local t=$1 m=$2
  git -C $MAIN worktree add -q -b "$t" "$W/$t" main 2>/dev/null || git -C $MAIN worktree add -q "$W/$t" "$t"
  local BASE; BASE=$(git -C $MAIN rev-parse "$t")
  local P; P=$(sed "s/__T__/$t/g; s/__M__/$m/g" queue/prompt.txt)
  ( cd $W/$t && agy -p "$P" --model "$m" --dangerously-skip-permissions < /dev/null > $W/logs/$t.log 2>&1
    if [ "$(git -C $MAIN rev-parse "$t")" != "$BASE" ] && git -C $MAIN merge-base --is-ancestor "$t" main; then
      git -C $MAIN worktree remove --force "$W/$t"; git -C $MAIN branch -D "$t"; echo "BITTI $t MERGED" >> $W/logs/$t.log
    else echo "BITTI $t MERGE_EDILMEDI (worktree korundu: $W/$t)" >> $W/logs/$t.log; fi ) &
  echo "$(date +%H:%M) basladi $t $m"; sleep 10; }
while :; do
  pending=0
  while read -r t m d; do
    [ -z "$t" ] || [[ $t == \#* ]] && continue
    done_in_main "$t" && continue
    [ -d "$W/$t" ] && continue            # çalışıyor ya da korunmuş
    pending=1; ok=1
    for x in ${d//,/ }; do done_in_main "$x" || ok=0; done
    [ $ok = 1 ] && [ "$(pgrep -x agy | wc -l)" -lt "$MAX" ] && launch "$t" "$m"
  done < queue/items2.txt
  [ $pending = 0 ] && [ "$(pgrep -x agy | wc -l)" -le 1 ] && break
  sleep 60
done
echo "KUYRUK BITTI $(date)"
