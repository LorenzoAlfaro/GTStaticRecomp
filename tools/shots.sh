#!/bin/sh
# Headless run with a screenshot every N frames, combined into build/montage.png.
#   tools/shots.sh <frames> <every> [extra gt.exe args...]
cd "$(dirname "$0")/.."
frames=$1; every=$2; shift 2
rm -rf build/shots && mkdir -p build/shots
timeout 300 ./gt.exe --display 1 --turbo --frames "$frames" --shot-every "$every" build/shots "$@" > build/run_out.txt 2> build/run_err.txt
echo "exit=$?"
pwsh -NoProfile -File tools/montage.ps1 -Dir build/shots -Out build/montage.png
