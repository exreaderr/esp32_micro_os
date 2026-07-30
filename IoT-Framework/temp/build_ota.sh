#!/bin/bash
# =================================================================================
#                 IoT BUILD MASTER DEVOPS CONVEYOR v11.5 (FINAL PRODUCTION)
# =================================================================================
# Аргументы, передаваемые из Python-инжектора:
# 1:SRC_DIR, 2:HA_DIR, 3:FS_SIZE, 4:STEP, 5:PROJ_ID, 6:MQTT_IP
# 7:DO_FS, 8:M_USER, 9:M_PASS, 10:CHLOG, 11:GUI_VER, 12:BIN_DIR
# =================================================================================

PROJ_DIR="${1}";  HA_DIR="${2}";     FS_SIZE="${3}";  STEP="${4}";    PROJ_ID="${5}"
MQTT_IP="${6}";   DO_FS="${7}";      M_USER="${8}";   M_PASS="${9}";  CHLOG="${10}"
GUI_VER="${11}";  BIN_DIR="${12}"

# =================================================================================
# ЖЕСТКАЯ ФИКСАЦИЯ АБСОЛЮТНЫХ ПУТЕЙ К ИНСТРУМЕНТАМ ESP32
# =================================================================================
MKLITTLEFS="/home/kiv/.arduino15/packages/esp32/tools/mklittlefs/4.0.2-db0513a/mklittlefs"

# ИСПРАВЛЕНИЕ: Привязываем рабочий путь к основной переменной ARDUINO_CLI_BIN
ARDUINO_CLI_BIN="/home/kiv/.arduino-IDE_2.3.8/resources/app/lib/backend/resources/arduino-cli"

# Резервная проверка путей (сравниваем правильную переменную!)
if [ ! -f "$ARDUINO_CLI_BIN" ]; then
 ARDUINO_CLI_BIN="/home/kiv/.arduino-IDE_2.3.8/resources/app/node_modules/arduino-cli/arduino-cli"
fi

if [ ! -f "$ARDUINO_CLI_BIN" ]; then
 ARDUINO_CLI_BIN="arduino-cli"
fi

# Создаем изолированную временную директорию сборки статики с автоудалением
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT

echo "[INFO] 🔍 Сканирование папки проекта..."
# Находим главный файл .ino проекта
IFS= read -r INO_FILE < <(find "${PROJ_DIR}" -maxdepth 1 -name "*.ino" -print -quit)

if [ -z "$INO_FILE" ]; then
    echo "[ERROR] ❌ Критическая ошибка: В папке проекта не найден файл .ino!"
    exit 1
fi

INO_NAME=$(basename "$INO_FILE" .ino)
echo "[INFO] 🛠️ Обнаружен проект: ${INO_NAME}.ino"

# Принудительно создаем целевую папку build
mkdir -p "${BIN_DIR}"

# --- БЛОК АВТОМАТИЧЕСКОЙ КОМПИЛЯЦИИ ЯДРА ---
echo "[INFO] ⚡ Запуск компилятора: $ARDUINO_CLI_BIN"
echo "[INFO] ⏳ Пожалуйста, подождите, идет полная сборка C++ бинарников..."

# Запуск сборки с флагами вывода бинарников напрямую в локальную папку build проекта
"$ARDUINO_CLI_BIN" compile --fqbn esp32:esp32:wt32-eth01 --export-binaries --output-dir "${BIN_DIR}" "${PROJ_DIR}"

if [ $? -ne 0 ]; then
    echo "[ERROR] ❌ Критическая ошибка: Компиляция скетча завершилась сбоем!"
    exit 1
fi

# Находим свежескомпилированный файл прошивки
FW_FILE=$(find "${BIN_DIR}" -type f \( -name "${INO_NAME}.bin" -o -name "${INO_NAME}.ino.bin" \) ! -name "*.merged.bin" ! -name "*.partitions.bin" -printf "%T@ %p\n" | sort -n | tail -1 | awk '{print $2}')

if [ ! -f "$FW_FILE" ]; then
    echo "[ERROR] ❌ Критическая ошибка: Бинарник прошивки не обнаружен в build после компиляции!"
    exit 1
fi

echo "[SUCCESS] 📦 Сборка ядра успешно завершена: $(basename "$FW_FILE")"
cd "$PROJ_DIR" || exit 1

# --- Автоматический расчет инкремента версий прошивки ---
MAJ=$(echo "$GUI_VER" | cut -d. -f1)
MIN=$(echo "$GUI_VER" | cut -d. -f2)
PAT=$(echo "$GUI_VER" | cut -d. -f3)
[ -z "$PAT" ] && PAT=0

if [ "$STEP" == "0.1" ]; then
    NEW_PAT=$((PAT + 1)); NEW_MIN=$MIN; NEW_MAJ=$MAJ
else
    NEW_PAT=0; NEW_MIN=$((MIN + 1)); NEW_MAJ=$MAJ
fi

NEW_VER="${NEW_MAJ}.${NEW_MIN}.${NEW_PAT}"
echo "[INFO] 📈 Новая версия релиза: v$NEW_VER"

# Создаем базовый пустой манифест локально, если его еще нет
if [ ! -f "version.json" ]; then
    echo '{"version":"0.0.0","fw_md5":"","fs_md5":"","changelog":""}' > version.json
fi

# --- Подготовка, Gzip-сжатие и компиляция LittleFS ---
if [ "$DO_FS" = "true" ]; then
    echo "[INFO] 🔨 Сборка файловой системы LittleFS..."
    if [ -d "./data" ]; then
        cp -r ./data/* "$TEMP_DIR/"

        # Автоматическое сжатие статики Mushroom интерфейса усадьбы
        find "$TEMP_DIR" -type f \( -name "*.js" -o -name "*.css" -o -name "*.html" \) ! -name "*.gz" -exec gzip -f9 {} \;

        # Проверка лимитов веса данных перед упаковкой LittleFS сектора
        CURRENT_DATA_SIZE=$(du -sb "$TEMP_DIR" | awk '{print $1}')
        if [ "$CURRENT_DATA_SIZE" -gt "$FS_SIZE" ]; then
            echo "[ERROR] ❌ ОШИБКА: Вес файлов ($CURRENT_DATA_SIZE б) превышает лимит раздела LittleFS ($FS_SIZE б)!"
            exit 1
        fi

        # Сборка бинарного LittleFS образа статики
        "$MKLITTLEFS" -c "$TEMP_DIR" -p 256 -b 4096 -s "$FS_SIZE" littlefs.bin

        # Расчет MD5-хеша LittleFS строго в нижнем регистре
        FS_MD5=$(md5sum littlefs.bin | awk '{print $1}' | tr 'A-Z' 'a-z')
        echo "[SUCCESS] ✅ Бинарный образ LittleFS успешно сформирован."
    else
        echo "[WARN] ⚠️ Директория ./data не обнаружена, сборка LittleFS пропущена."
        DO_FS="false"
        FS_MD5=$(jq -r '.fs_md5 // ""' version.json | tr 'A-Z' 'a-z')
    fi
else
    FS_MD5=$(jq -r '.fs_md5 // ""' version.json | tr 'A-Z' 'a-z')
fi

# --- Расчет MD5 прошивки и обновление манифеста version.json ---
FW_MD5=$(md5sum "$FW_FILE" | awk '{print $1}' | tr 'A-Z' 'a-z')

jq --arg nv "$NEW_VER" --arg fw "$FW_MD5" --arg fs "$FS_MD5" --arg cl "$CHLOG" \
  '.version = $nv | .fw_md5 = $fw | .fs_md5 = $fs | .changelog = $cl' \
  version.json > temp.json && mv temp.json version.json

# --- Ротация бэкапов истории и деплой на сервер Home Assistant ---
if [ -d "$HA_DIR" ]; then
    echo "[INFO] 📂 Запущена ротация резервных копий в папке ОТА HA..."
    mkdir -p "$HA_DIR/backup" "$HA_DIR/archive"

    if [ -f "$HA_DIR/backup/version.json" ]; then
        B_VER=$(jq -r '.version' "$HA_DIR/backup/version.json")
        TS=$(date +%Y%m%d_%H%M)
        tar -czf "$HA_DIR/archive/v${B_VER}_${TS}.tar.gz" -C "$HA_DIR/backup" . 2>/dev/null
        echo "[INFO] 📦 Прошлый бэкап v$B_VER отправлен в архив истории."
    fi

    # Сдвигаем текущие рабочие файлы в папку backup
    [ -f "$HA_DIR/version.json" ]  && mv "$HA_DIR/version.json"  "$HA_DIR/backup/"
    [ -f "$HA_DIR/firmware.bin" ]  && mv "$HA_DIR/firmware.bin"  "$HA_DIR/backup/"
    [ -f "$HA_DIR/littlefs.bin" ]  && mv "$HA_DIR/littlefs.bin"  "$HA_DIR/backup/"

    # Выкладываем свежий пак релиза в PRODUCTION
    cp "$FW_FILE" "$HA_DIR/firmware.bin"
    cp version.json "$HA_DIR/"
    [ "$DO_FS" = "true" ] && cp littlefs.bin "$HA_DIR/littlefs.bin"

    echo "[SUCCESS] ✅ Релиз v$NEW_VER успешно доставлен на сервер Home Assistant."
else
    echo "[ERROR] ❌ Критическая ошибка: Сетевой путь деплоя Home Assistant $HA_DIR недоступен!"
    exit 1
fi

# --- Отправка фрейма готовности релиза в брокер MQTT ---
AUTH=""
if [ -n "$M_USER" ]; then
    AUTH="-u $M_USER -P $M_PASS"
fi

mosquitto_pub -h "$MQTT_IP" $AUTH -t "homeassistant/ota/$PROJ_ID" -m "{\"version\": \"$NEW_VER\", \"status\": \"ready\"}" 2>/dev/null
echo "[SUCCESS] 🏁 DevOps-конвейер автосборки завершен успешно."
