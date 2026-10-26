#!/usr/bin/env python3
"""tools/embed.py — shaders/*.mtk dosyalarını wasm'a gömülecek bir C dosyasına çevirir.
Kullanım: embed.py çıktı.c girdi1.mtk girdi2.mtk ...
Üretilen semboller: src/core/embed.h
"""
import os, sys

out, files = sys.argv[1], sorted(sys.argv[2:])
lines = ['/* OTOMATİK ÜRETİLDİ (tools/embed.py) — elle düzenleme. */',
         '#include "core/embed.h"', '']
for i, path in enumerate(files):
    data = open(path, 'rb').read() + b'\0'
    body = ','.join(str(b) for b in data)
    lines.append(f'static const char g_emb_{i}[{len(data)}] = {{{body}}};')
lines.append('')
lines.append(f'const EmbeddedFile g_embedded_shaders[{max(len(files), 1)}] = {{')
for i, path in enumerate(files):
    name = os.path.basename(path)
    size = os.path.getsize(path)
    lines.append(f'    {{ "{name}", g_emb_{i}, {size}u }},')
if not files:
    lines.append('    { "", "", 0u },')
lines.append('};')
lines.append(f'const u32 g_embedded_shader_count = {len(files)}u;')
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, 'w').write('\n'.join(lines) + '\n')
