#!/bin/bash
# tools/play.sh — MATOKI'yi WebGPU (gerçek GPU, Vulkan) açık Chrome'da başlatır.
# Linux Chrome WebGPU adaptörünü bayraksız vermez; açık bir Chrome bayrakları yok saydığı için
# ayrı profil kullanılır. Sunucu kapalıysa tools/serve.py başlatılır.
cd "$(dirname "$0")/.."
if ! curl -s -o /dev/null http://localhost:8080/; then
    nohup python3 tools/serve.py >/dev/null 2>&1 &
    sleep 0.5
fi
exec google-chrome --user-data-dir="$HOME/.cache/matoki-chrome" --no-first-run \
    --enable-unsafe-webgpu --enable-features=Vulkan --use-angle=vulkan --ignore-gpu-blocklist \
    "http://localhost:8080/${1:+?scene=$1}"
