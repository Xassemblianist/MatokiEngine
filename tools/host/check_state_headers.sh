#!/usr/bin/env bash
# tools/host/check_state_headers.sh — Durum başlıklarında (state_*.h) işaretçi alanı yasağı denetimi (D-112, S1).
#
# Kurallar:
#  1) state_*.h içindeki hiçbir struct veya union tanımında ham işaretçi alanı ('*') veya
#     fonksiyon işaretçisi alanı bulunamaz.
#  2) state_*.h içindeki hiçbir struct veya union tanımında platforma göre boyutu değişen
#     tamsayı tipleri (usize, isize, uptr, size_t, uintptr_t, ptrdiff_t, long) bulunamaz.
#  3) state_*.h içinde işaretçi tipi üreten typedef (typedef ... * ...) bulunamaz.
#  4) Her state_*.h başlığında derleme zamanı doğrulaması (_Static_assert / STATE_ASSERT_*) bulunmalıdır.
#
# Kullanım:
#   tools/host/check_state_headers.sh [dosya1.h dosya2.h ...]
# Argüman verilmezse src/ altındaki tüm state_*.h dosyalarını tarar.

set -euo pipefail

if [ "$#" -gt 0 ]; then
    FILES=("$@")
else
    mapfile -t FILES < <(find src -type f -name 'state_*.h' | sort)
fi

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "HATA: check_state_headers.sh: denetlenecek state_*.h dosyasi bulunamadi" >&2
    exit 1
fi

python3 - "${FILES[@]}" <<'PYEOF'
import re
import sys

FORBIDDEN_ARCH_TYPES = {
    "usize", "isize", "uptr",
    "size_t", "ssize_t", "uintptr_t", "intptr_t", "ptrdiff_t", "long"
}

def strip_comments_and_strings(src: str) -> str:
    """Yorumları ve dizgileri satır numaralarını koruyarak boşlukla değiştirir."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                out.append(' ')
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            out.append(' ')
            out.append(' ')
            i += 2
            while i < n and not (src[i] == '*' and i + 1 < n and src[i + 1] == '/'):
                out.append('\n' if src[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append(' ')
                out.append(' ')
                i += 2
        elif c in ('"', "'"):
            quote = c
            out.append(' ')
            i += 1
            while i < n and src[i] != quote:
                if src[i] == '\\' and i + 1 < n:
                    out.append('\n' if src[i] == '\n' else ' ')
                    i += 1
                out.append('\n' if src[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append(' ')
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)

def blank_preprocessor_directives(src: str) -> str:
    """Önişlemci (#define vb.) satırlarını satır sayısını bozmadan boşlukla maskeler."""
    lines = src.split('\n')
    out_lines = []
    in_directive = False
    for line in lines:
        stripped = line.lstrip()
        if in_directive or stripped.startswith('#'):
            cont = line.endswith('\\')
            out_lines.append('')
            in_directive = cont
        else:
            out_lines.append(line)
    return '\n'.join(out_lines)

def check_file(path: str) -> list[str]:
    errors = []
    try:
        with open(path, 'r', encoding='utf-8') as f:
            raw = f.read()
    except OSError as e:
        return [f"{path}:1: dosya okunamadi: {e}"]

    no_comments = strip_comments_and_strings(raw)

    # Kural 4: En az bir _Static_assert veya STATE_ASSERT_* bulunmalı
    if not re.search(r'\b(_Static_assert|STATE_ASSERT_NO_PTR_FIELD|STATE_ASSERT_VALUE_STRUCT|STATE_REF_DEFINE|STATE_HANDLE_DEFINE)\b', no_comments):
        errors.append(f"{path}:1: state_*.h basliginda _Static_assert / STATE_ASSERT_* dogrulamasi eksik")

    code = blank_preprocessor_directives(no_comments)

    # Satır başlangıç ofsetleri (ofset -> satır numarası dönüşümü için)
    line_starts = [0]
    for idx, ch in enumerate(code):
        if ch == '\n':
            line_starts.append(idx + 1)

    def offset_to_line(off: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if line_starts[mid] <= off:
                lo = mid + 1
            else:
                hi = mid - 1
        return hi + 1

    # Kural 1 & 2: struct / union gövdelerini bul ve alan bildirimlerini denetle
    pattern = re.compile(r'\b(struct|union)\b(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*\{')
    pos = 0
    n = len(code)
    while pos < n:
        m = pattern.search(code, pos)
        if not m:
            break
        brace_open = m.end() - 1
        depth = 1
        i = brace_open + 1
        field_start = i
        while i < n and depth > 0:
            ch = code[i]
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
            elif ch == ';' and depth >= 1:
                decl = code[field_start:i]
                decl_clean = decl.strip()
                if decl_clean:
                    line_no = offset_to_line(field_start + (len(decl) - len(decl.lstrip())))
                    # Kural 1: İşaretçi alanı ('*') yasağı (dizi boyutu ifadesindeki [a * b] hariç tutulmaz,
                    # fakat yine de köşeli parantez içini maskeleyerek gerçek işaretçi '*' kontrolü yapalım)
                    decl_no_brackets = re.sub(r'\[[^\]]*\]', '[]', decl_clean)
                    if '*' in decl_no_brackets:
                        errors.append(
                            f"{path}:{line_no}: HATA: durum yapisinda (struct/union) isaretci alani yasak: '{' '.join(decl_clean.split())};'"
                        )
                    # Kural 2: Platforma bağlı boyutlu tip yasağı
                    tokens = re.findall(r'\b[A-Za-z_][A-Za-z0-9_]*\b', decl_no_brackets)
                    for tok in tokens:
                        if tok in FORBIDDEN_ARCH_TYPES:
                            errors.append(
                                f"{path}:{line_no}: HATA: durum yapisinda platforma bagli boyutlu tip '{tok}' yasak (u32/u64 kullanin): '{' '.join(decl_clean.split())};'"
                            )
                field_start = i + 1
            i += 1
        pos = i

    # Kural 3: Üst düzeyde işaretçi typedef'i yasağı (typedef ... * ...;)
    for tm in re.finditer(r'\btypedef\b([^;{}]+);', code):
        body = tm.group(1)
        body_no_brackets = re.sub(r'\[[^\]]*\]', '[]', body)
        if '*' in body_no_brackets:
            line_no = offset_to_line(tm.start())
            errors.append(
                f"{path}:{line_no}: HATA: state_*.h icinde isaretci typedef'i yasak: 'typedef {' '.join(body.split())};'"
            )

    return errors

all_errors = []
for fpath in sys.argv[1:]:
    all_errors.extend(check_file(fpath))

if all_errors:
    for err in all_errors:
        print(err, file=sys.stderr)
    sys.exit(1)

sys.exit(0)
PYEOF
