#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# gen_docs.py — ГЕНЕРАТОР СПРАВОЧНИКА ПЛАТФОРМЫ (D3)
# ============================================================================
# Принцип D3: "документы = код". Справочник собирается ИЗ ИСХОДНИКОВ
# (реестр событий, регистрация модулей, конфиг-схемы, маршруты HTTP),
# поэтому не может устареть: устаревает — перегенерируется.
#
# Запуск:  python3 tools/gen_docs.py
# Выход:   ../МикроОС_5.0_справочник_платформы.md (рядом с проектом)
# ============================================================================
import re
import os
import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # micro-os-5
OUT = os.path.join(os.path.dirname(ROOT),
                   "МикроОС_5.0_справочник_платформы.md")


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


# ============================================================================
# 1. РЕЕСТР СОБЫТИЙ (core/Events.h)
# ============================================================================
def parse_events():
    text = read("src/core/Events.h")
    events = []
    # Запись вида:  NAME = 0xXXXX,  // комментарий (возможно многострочный)
    pattern = re.compile(
        r"^\s*([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+),\s*"
        r"//\s*(.*?)(?=^\s*[A-Z][A-Z0-9_]+\s*=|^\s*};|^\s*//\s*=)",
        re.M | re.S)
    for m in pattern.finditer(text):
        name, eid, comment = m.group(1), m.group(2), m.group(3)
        # Склеить многострочный комментарий в одну строку
        comment = re.sub(r"\s*//\s*", " ", comment).strip()
        # Отрезать прилипший заголовок следующей секции ("Xxx (0x....–0x....)")
        comment = re.split(r"\s[A-Z][A-Za-z]+\s*\(0x[0-9A-Fa-f]{4}–", comment)[0]
        comment = comment.strip()
        events.append((name, int(eid, 16), comment))
    return sorted(events, key=lambda e: e[1])


# ============================================================================
# 2. МОДУЛИ ЯДРА (core/Kernel.cpp — блок регистрации)
# ============================================================================
def parse_modules():
    text = read("src/core/Kernel.cpp")
    modules = []
    pattern = re.compile(
        r"registerModule\(&(\w+)::getInstance\(\),\s*/\*prio\*/\s*(\d+)")
    for m in pattern.finditer(text):
        modules.append((m.group(1), int(m.group(2))))
    return modules


# ============================================================================
# 3. КОНФИГ-СХЕМЫ (addFields во всех модулях)
# ============================================================================
CONFIG_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*ConfigType::(\w+)\s*,\s*"([^"]*)"\s*,\s*'
    r'(-?\d+)\s*,\s*(-?\d+)\s*,\s*([^,]+?)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"'
    r'\s*\}',
    re.S)


def parse_config_fields():
    fields = []
    sources = []
    for sub in ("src/services", "projects/smart_lock/src"):
        d = os.path.join(ROOT, sub)
        for fn in sorted(os.listdir(d)):
            if fn.endswith((".cpp", ".h")):
                sources.append(os.path.join(sub, fn))
    for rel in sources:
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            text = f.read()
        for m in CONFIG_RE.finditer(text):
            key, typ, default, mn, mx, flags, group, label = m.groups()
            flags = re.sub(r"\s+", "", flags)
            fields.append({
                "key": key, "type": typ, "default": default,
                "min": mn, "max": mx, "flags": flags,
                "group": group, "label": label,
                "source": os.path.basename(rel),
            })
    return sorted(fields, key=lambda f: (f["group"], f["key"]))


# ============================================================================
# 4. HTTP-МАРШРУТЫ (services/HttpService.cpp)
# ============================================================================
def parse_routes():
    text = read("src/services/HttpService.cpp")
    routes = []
    pattern = re.compile(
        r'_server\.on\("([^"]+)"\s*,\s*HTTP_(\w+)\s*,.*?(\w+)\(\);\s*}')
    for m in pattern.finditer(text):
        routes.append((m.group(3), m.group(1), m.group(2)))
    return routes


# ============================================================================
# СБОРКА ДОКУМЕНТА
# ============================================================================
def main():
    events = parse_events()
    modules = parse_modules()
    fields = parse_config_fields()
    routes = parse_routes()

    today = datetime.date.today().isoformat()
    L = []
    L.append("# МикроОС 5.0 — Справочник платформы (сгенерировано)\n")
    L.append("> **Документ сгенерирован автоматически** из исходного кода "
             f"скриптом `tools/gen_docs.py` ({today}).\n"
             "> Не редактировать вручную — изменения вносить в код и "
             "перегенерировать (принцип D3: «документы = код»).\n")

    # --- События -------------------------------------------------------------
    L.append("\n## 1. Реестр событий шины (core/Events.h)\n")
    L.append("Диапазоны: `0x0000–0x00FF` ядро · `0x0100+` сервисы · "
             "`0x0200+` транспорт · `0x0300+` драйверы · `0x0400+` домен · "
             "`0x0900+` команды · `0x1000+` приложения профилей.\n")
    L.append("| ID | Событие | Назначение |")
    L.append("|---|---|---|")
    for name, eid, comment in events:
        L.append(f"| `0x{eid:04X}` | `{name}` | {comment} |")

    # --- Модули ---------------------------------------------------------------
    L.append("\n## 2. Модули ядра (порядок регистрации в Kernel)\n")
    L.append("| Модуль | Приоритет |")
    L.append("|---|---|")
    for name, prio in modules:
        L.append(f"| `{name}` | {prio} |")
    L.append("\nПриоритет определяет порядок фаз describe → "
             "registerExtensions → init → start. Меньше — раньше.")

    # --- Конфиг-схемы -----------------------------------------------------------
    L.append("\n## 3. Конфигурационная схема (инжекция модулей)\n")
    L.append("Все поля, зарегистрированные модулями через "
             "`ConfigService::addFields`. Веб-UI администратора строится "
             "по этой таблице автоматически; `SECRET` не покидает устройство.\n")
    L.append("| Ключ | Тип | Умолчание | Мин | Макс | Флаги | "
             "Группа | Описание | Модуль |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for f in fields:
        L.append(f"| `{f['key']}` | {f['type']} | `{f['default']}` | "
                 f"{f['min']} | {f['max']} | {f['flags']} | "
                 f"{f['group']} | {f['label']} | {f['source']} |")

    # --- HTTP --------------------------------------------------------------------
    L.append("\n## 4. HTTP-эндпоинты (HttpService)\n")
    L.append("| Обработчик | Путь | Метод |")
    L.append("|---|---|---|")
    for handler, path, method in routes:
        L.append(f"| `{handler}` | `{path}` | {method} |")
    L.append("\nПлюс префиксный маршрут `/api/dev/*` — профильный API "
             "(IUiProvider). Админские эндпоинты требуют токен "
             "(`X-Auth-Token` или `?token=`).")

    # --- MQTT (справочно, из MqttTransport) ----------------------------------------
    L.append("\n## 5. Топологию MQTT (MqttTransport, E2)\n")
    L.append("Префикс `mqtt.prefix` (умолчание `microos`), `<id>` — MAC ETH.\n")
    L.append("| Топик | Направление | Назначение |")
    L.append("|---|---|---|")
    L.append("| `<pfx>/<id>/state` | publish (retain, LWT) | online/offline |")
    L.append("| `<pfx>/<id>/telemetry` | publish (retain) | JSON-снимок B1 |")
    L.append("| `<pfx>/<id>/events/<NAME>` | publish | зеркало событий шины |")
    L.append("| `<pfx>/<id>/cmd/<verb>` | subscribe | команды: reboot, state, set, ota |")
    L.append("| `<pfx>/all/cmd/<verb>` | subscribe | broadcast-команды |")
    L.append("| `<pfx>/+/events/#` | subscribe | события других устройств -> шина |")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print(f"OK: {OUT}")
    print(f"  событий: {len(events)}, модулей: {len(modules)}, "
          f"полей конфига: {len(fields)}, маршрутов: {len(routes)}")


if __name__ == "__main__":
    main()
