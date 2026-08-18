# Прошивка мастера умного дома home_master (МикроОС 5.7.0, этап M3.2)

Железо: **Waveshare ESP32-S3-POE-ETH** (ESP32-S3R8: 8 МБ PSRAM, 16 МБ flash,
W5500 + PoE, слот microSD). DS3231 на I2C предусмотрен обвязкой
(SDA=GPIO16, SCL=GPIO17, адрес 0x68 — та же, что у smart_lock), но пока
**не распаян** — время мастер держит по NTP. Питание — PoE и/или ИБЖ
(DC-DC + АКБ, схема smart_counter).

Роль в контуре (LAN 10.146.75.0/25, статический DHCP **10.146.75.54**):
встроенный MQTT-брокер + мост M2 к вышестоящему брокеру HA
(10.146.75.5:1883), журнал событий парка на SD с вьюером в панели,
ОТА-клиент. Замок — 10.146.75.53.

## Состав комплекта

```
arduino/home_master/
├── firmware/
│   ├── home_master_full_16mb.bin   — ПОЛНЫЙ образ 16 МБ (загрузчик+таблица+
│   │                                 ядро+профиль+панель) — один файл на 0x0
│   ├── littlefs_hm.bin             — образ LittleFS с панелью (0xC90000)
│   ├── partitions.csv              — разметка: app 2×6.5 МБ, FS 3.4 МБ
│   └── home_master.ino.bin, .bootloader.bin, .partitions.bin, .elf, .map
└── ИНСТРУКЦИЯ_ПО_ПРОШИВКЕ_home_master.md — этот файл

Библиотека ядра 5.7.0 — общая: arduino/libraries/MicroOS (та же библиотека
собирает smart_lock — парк живёт на одном ядре). Проект IDE:
arduino/home_master_project/ (или home_master_project.zip).

ОТА-зеркало для HA: arduino/ota/home_master/ (version.json + firmware.bin +
littlefs.bin) — раскладывается Build Master-ом; готовый архив —
arduino/ota_home_master_5.7.0.zip.
```

> **Обновление по воздуху (штатный путь):** действующий мастер обновляется
> ОТА через вкладку «Система» панели. Если в релизе менялась панель —
> режим «прошивка+ФС» (сказано в changelog манифеста). Полная прошивка
> по UART — только для нового/восстановленного устройства.

## Вариант А — готовый образ (быстрый старт)

Подключение: **USB-C** напрямую (переходники не нужны). Перевод в
download-режим, если плата не вошла сама: **удерживать BOOT → кратко
нажать RESET → отпустить BOOT**.

```bash
# Полный образ (прошивка + панель) — одной командой:
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 home_master_full_16mb.bin

# Или по частям:
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 write-flash \
  0x0      home_master.ino.bootloader.bin \
  0x8000   home_master.ino.partitions.bin \
  0xe000   boot_app0.bin \
  0x10000  home_master.ino.bin \
  0xc90000 littlefs_hm.bin
```

> **Порт под Windows/Linux:** S3 с USB-CDC обычно виден как `/dev/ttyACM0`
> (не ttyUSB!). Известная болячка S3: COM-порт может пропадать после
> прошивки — лечится переводом в download-режим (BOOT+RESET) и повтором.
> Монитор порта: 115200, прошивка собрана с USB CDC On Boot = Enabled —
> лог загрузки идёт в тот же USB-C.

## Вариант Б — Arduino IDE (из исходников)

1. Библиотека `libraries/MicroOS` (версия 5.7.0) → в `~/Arduino/libraries/`.
2. Открыть `home_master_project/home_master.ino`.
3. Плата: **Tools → Board → esp32 → «ESP32S3 Dev Module»** (отдельной
   платы Waveshare в списке нет и не нужно — вся специфика платы задана
   в ядре через BoardDesc).
4. Настройки в Tools (ядро esp32:esp32 3.3.11):
   - USB CDC On Boot: **Enabled**;
   - Flash Size: **16MB (128Mb)**;
   - PSRAM: **OPI PSRAM** (у S3R8 — восьмибитная!);
   - Partition Scheme: **Custom** — тогда IDE берёт `partitions.csv`
     ИЗ ПАПКИ СКЕТЧА (он уже лежит рядом с home_master.ino: app 2×6.5 МБ
     OTA + FS 3.4 МБ). Это приоритетный механизм ядра Arduino-ESP32:
     partitions.csv из папки скетча перекрывает любую схему меню.
5. Скетч → Загрузить. Панель — образом `littlefs_hm.bin` (команда выше)
   или плагином LittleFS Data Upload.

## Вариант В — arduino-cli

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc" \
  --build-property "build.partitions=default_16MB" \
  --libraries ./libraries ./home_master_project
arduino-cli upload --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc" \
  -p /dev/ttyACM0 ./home_master_project
```

## Проверка после прошивки (приёмка 5.7.0)

1. В логе загрузки: `[KERNEL] MicroOS 5.7.0 boot, profile: home_master`,
   `[KERNEL] board: ESP32-S3-POE-ETH`, `SD mounted: SDHC, ...`,
   `init: 67 fields registered`, строка журнала
   (`journal: ON | фильтр ... | сброс 10 с | сегмент 100 МБ | глубина 90 дн`).
2. Кабель Ethernet в PoE-коммутатор (или обычный — питание тогда по USB-C/
   ИБЖ): адрес статический по DHCP — **10.146.75.54** (см. лог или роутер).
3. Браузер: `http://10.146.75.54/` — статус мастера;
   `http://10.146.75.54/api/dev/hm/info` — JSON сводки (плата, SD, режим,
   память, блок journal).
4. Панель (пароль задаётся при первом старте): вкладки Статус / Сеть /
   MQTT / **Журнал** / Настройки / ПАЗ / Система. Во вкладке «Журнал» —
   список сегментов SD, фильтры (источник/текст/период), кнопка «Ещё»,
   «следить» (живой хвост, 5 с).
5. MQTT: встроенный брокер слушает 1883, мост M2 — к HA (10.146.75.5:1883);
   в бут-логе оба состояния, на вкладке «MQTT» — живые счётчики.
6. ПАЗ: раздел «Здоровье модулей» — проверка `hm.sd` (Ok/Warning/Critical).

## Safe Mode

Кнопка **BOOT** (GPIO0) — нажать ПОСЛЕ начала загрузки (удержание BOOT в
момент сброса = download-режим ROM, это прошивка, а не Safe Mode).
Остальные триггеры (bootloop, команда) — ядерные, как у smart_lock.
