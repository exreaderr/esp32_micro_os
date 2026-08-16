#!/bin/sh
# run_tests.sh — ворота качества №1: хост-тесты чистой логики МикроОС.
# Норма на 16.08.2026 (ядро 5.8.0): 43351 PASS / 0 FAIL. Меньше — стоп.
# Запуск: cd MicroOS/host && ./run_tests.sh
set -e
cd "$(dirname "$0")"
OUT=/tmp/microos_host_tests
echo "== build host tests =="
g++ -std=c++17 -I shim tests.cpp ../src/core/ResourceManager.cpp -o "$OUT"
echo "== run =="
"$OUT"
