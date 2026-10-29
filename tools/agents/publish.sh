#!/bin/bash
# main'i co-author satırları temizlenmiş olarak github.com/Xassemblianist/MatokiEngine'e gönderir.
# filter-branch belirlenimci (aynı içerik → aynı hash), bu yüzden her çalıştırma hızlı ileri sarma pushu olur.
set -e
EX=/home/ostriquetrum/matoki-wt/publish/MatokiEngine
rm -rf "$EX"; mkdir -p "$(dirname $EX)"
git clone -q --no-local --single-branch --branch main /home/ostriquetrum/WEBASSEMBLY "$EX"
cd "$EX"; git remote remove origin
FILTER_BRANCH_SQUELCH_WARNING=1 git filter-branch --env-filter 'export GIT_AUTHOR_EMAIL=omerkaan20102003@gmail.com GIT_COMMITTER_EMAIL=omerkaan20102003@gmail.com' --msg-filter 'sed -E "/^[Cc]o-[Aa]uthored-[Bb]y:/d; /Generated with \[Claude Code\]/d" | git stripspace' -- main >/dev/null
test "$(git log --format=%B | grep -ciE 'co-authored|claude code')" = 0
git remote add origin https://github.com/Xassemblianist/MatokiEngine.git
git push ${FORCE:+--force} origin main
