#!/bin/bash
# Özerk kuyruk: items2.txt satırları "görev model [bağımlılık,...]". Bağımlılıklar main'de DONE olunca başlar.
# Her ajan kendi worktree'sinde çalışır ve merge.sh ile kendisi main'e alır + yayınlar. Aynı anda en çok MAX ajan.
MAX=${MAX:-5}  # agy hub süreci de sayılır → 4 ajan; W=/home/ostriquetrum/matoki-wt; MAIN=/home/ostriquetrum/WEBASSEMBLY; cd $W
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
