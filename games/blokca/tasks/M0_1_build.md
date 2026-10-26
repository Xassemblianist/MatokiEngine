DURUM: TODO

# M0_1 — Build sistemi ve çekirdek tipler

## Amaç
libc'siz, Emscripten'siz C → wasm32 derleme hattı.

## Dosyalar
- `Makefile`
- `src/core/types.h` — sabit genişlikli tipler, `static_assert`'ler, `ARRAY_LEN`, `MIN/MAX/CLAMP`
- `src/core/mem.c`, `src/core/mem.h` — `memcpy`, `memset`, `memmove`, `memcmp` (clang bunları örtük çağırır, mutlaka olmalı). `__builtin_memcpy` değil, bulk-memory kullanan döngüsüz implementasyon tercih.
- `src/core/arena.h`, `src/core/arena.c` — `arena_init`, `arena_push(a, size, align)`, `arena_reset`, `arena_mark/arena_pop_to`
- `src/main.c` — export'lar (şimdilik boş gövdeler), `__heap_base` ile perm/frame arena kurulumu
- `tools/serve.py` — COOP/COEP başlıklı statik sunucu (`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`), `application/wasm` MIME

## Makefile gereksinimleri
- `CC=clang`, `--target=wasm32 -nostdlib -ffreestanding -fno-builtin -O2 -msimd128 -mbulk-memory -mnontrapping-fptoint -Wall -Wextra -Werror -std=c11`
- Link: `-Wl,--no-entry -Wl,--export-dynamic -Wl,--import-memory -Wl,--initial-memory=268435456 -Wl,--max-memory=1073741824 -Wl,--allow-undefined-file=src/plat/imports.syms` (veya `__attribute__((import_module("env")))`)
- `src/**/*.c` otomatik toplanır. Çıktı: `build/game.wasm`, `web/` içine kopyalanır.
- `make serve`, `make clean`.

## Kabul kriterleri
- `make` uyarısız geçer.
- `wasm-objdump -x build/game.wasm` (varsa) veya JS'te `WebAssembly.Module.exports` ile `game_init`, `game_frame` görünür.
- Çıktıda libc sembolü import edilmiyor (sadece `env.plat_*` ve `env.memory`).
