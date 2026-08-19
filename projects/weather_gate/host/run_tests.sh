#!/bin/sh
# run_tests.sh — ворота качества №1: хост-тесты чистой логики (ядро +
# профиль weather_gate). Норма на 19.08.2026 (ядро 5.8.1, W3):
# 43745 PASS / 0 FAIL. Меньше — стоп.
# Каноничная раскладка: репозиторий = { MicroOS/, projects/ }.
# Запуск: cd projects/weather_gate/host && ./run_tests.sh
set -e
cd "$(dirname "$0")"
OUT=/tmp/weather_gate_host_tests
echo "== build host tests =="
g++ -std=c++17 -I ../../../MicroOS/host/shim \
    tests.cpp ../../../MicroOS/src/core/ResourceManager.cpp -o "$OUT"
echo "== run =="
"$OUT"
