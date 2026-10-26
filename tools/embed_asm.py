#!/usr/bin/env python3
"""tools/embed_asm.py — asm/*.wat dosyalarını C dizgesi tablosu olarak gömer.
Kullanım: python3 tools/embed_asm.py src/bench/asm_sources.c asm/*.wat
Üretilen semboller: src/bench/asm_sources.h
"""
import os, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'src/bench/asm_sources.c'
files = sorted(sys.argv[2:])

lines = [
    '/* bench/asm_sources.c — OTOMATİK ÜRETİLDİ (tools/host/tool_wat2c veya tools/embed_asm.py).',
    ' * asm/ altındaki .wat dosyalarının gömülü WAT metinlerini ve arama yardımcısını içerir.',
    ' * Çalışma anında masm_assemble ile WebAssembly ikilisine derlenir.',
    ' */',
    '#include "bench/asm_sources.h"',
    ''
]

for i, path in enumerate(files):
    data = open(path, 'rb').read() + b'\0'
    body = ','.join(str(b) for b in data)
    lines.append(f'static const char g_asm_{i}[{len(data)}] = {{{body}}};')

lines.append('')
lines.append(f'const EmbeddedAsm g_embedded_asm[{max(len(files), 1)}] = {{')
for i, path in enumerate(files):
    name = os.path.basename(path)
    export_name = os.path.splitext(name)[0]
    size = os.path.getsize(path)
    lines.append(f'    {{ "{name}", "{export_name}", g_asm_{i}, {size}u }},')
if not files:
    lines.append('    { "", "", "", 0u },')
lines.append('};')
lines.append(f'const u32 g_embedded_asm_count = {len(files)}u;')
lines.append('')
lines.append('static b32 str_eq(const char* a, const char* b) {')
lines.append('    u32 i = 0;')
lines.append('    while (a[i] != \'\\0\' && b[i] != \'\\0\') {')
lines.append('        if (a[i] != b[i]) return FALSE;')
lines.append('        i++;')
lines.append('    }')
lines.append('    return a[i] == b[i];')
lines.append('}')
lines.append('')
lines.append('const EmbeddedAsm* bench_find_asm(const char* export_name) {')
lines.append('    if (!export_name) return NULL;')
lines.append('    for (u32 i = 0; i < g_embedded_asm_count; i++) {')
lines.append('        if (str_eq(g_embedded_asm[i].export_name, export_name) ||')
lines.append('            str_eq(g_embedded_asm[i].name, export_name)) {')
lines.append('            return &g_embedded_asm[i];')
lines.append('        }')
lines.append('    }')
lines.append('    return NULL;')
lines.append('}')

os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
open(out, 'w').write('\n'.join(lines) + '\n')
