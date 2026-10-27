#!/usr/bin/env bash
# tools/host/run_matoki_vs_wat.sh — Matoki JIT çıktısı ile elle yazılmış WAT gölgelendiricilerini
# derler, wasm-validate + mdis gidiş-dönüşünden geçirir, >=10.000 rastgele quad + bozuk girdilerle
# doğrular ve 1.000.000 quad üzerinde (>=15 tekrar medyan) Mquad/s verim tablosunu üretir.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

# 1. Host araçlarının (matc, masm, mdis) güncel olduğundan emin ol
make -s host

OUT_DIR="build/matoki_vs_wat"
mkdir -p "$OUT_DIR"

# 2. 4 shader için Matoki (.mtk) ve Elle WAT (asm/shade_*.wat) derlemesi + wasm-validate + mdis round-trip
for s in vcolor sprite plasma lit3d; do
    ./build/host/matc "shaders/${s}.mtk" -o "${OUT_DIR}/${s}_mtk.wasm"
    ./build/host/masm "asm/shade_${s}.wat" -o "${OUT_DIR}/${s}_wat.wasm"

    wasm-validate --enable-all "${OUT_DIR}/${s}_mtk.wasm"
    wasm-validate --enable-all "${OUT_DIR}/${s}_wat.wasm"

    ./build/host/mdis "${OUT_DIR}/${s}_mtk.wasm" -o "${OUT_DIR}/${s}_mtk.dis.wat"
    ./build/host/mdis "${OUT_DIR}/${s}_wat.wasm" -o "${OUT_DIR}/${s}_wat.dis.wat"

    # mdis -> masm ikili gidiş-dönüş (round-trip) doğrulaması
    ./build/host/masm "${OUT_DIR}/${s}_wat.dis.wat" -o "${OUT_DIR}/${s}_wat.rt.wasm"
    cmp -s "${OUT_DIR}/${s}_wat.wasm" "${OUT_DIR}/${s}_wat.rt.wasm"
done

# 3. Node/V8 üzerinde doğruluk (>=10.000 quad + bozuk girdi) ve 1M quad benchmark koşumu
node tools/host/bench_matoki_vs_wat.mjs "$@"
