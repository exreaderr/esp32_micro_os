#!/bin/sh
# run_tests.sh — ворота качества №1: хост-тесты чистой логики профиля
# smart_counter (C1). Счётчик PASS только растёт (брифинг §9).
# Каноничная раскладка: репозиторий = { MicroOS/, projects/ }.
# Запуск: cd projects/smart_counter/host && ./run_tests.sh
set -e
cd "$(dirname "$0")"
OUT=/tmp/smart_counter_host_tests
echo "== build host tests =="
g++ -std=c++17 -I ../../../MicroOS/host/shim \
    tests.cpp -o "$OUT"
echo "== run =="
"$OUT"
