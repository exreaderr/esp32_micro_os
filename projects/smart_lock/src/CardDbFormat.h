// ============================================================================
// CardDbFormat.h — ФОРМАТ БАЗЫ КАРТ СКУД (чистая логика, D2: host-тесты)
// ============================================================================
// users.json — совместим с монолитом smart_lock v2.5.0:
//   {"users":[{"id":"A1B2C3D4","name":"Иван","type":"permanent",
//              "track":7,"expiry":0,"pin":"4821"}, ...]}
// Строки типов — ТОЧНО как в монолите (getKeyTypeStr):
//   "master" | "permanent" | "temporary" | "one-time".
// pin — личный веб-ПИН жильца (4..6 цифр, СТРОГО уникальный; пусто/нет
// ключа = «Только карта», без веб-доступа). Уникальность — CardStore.
// uses/last_use/blocked (5.0.x) — статистика проходов и блокировка без
// удаления (идея AccessManager мёртвой ветки); пишутся только не-нулы,
// старые парсеры их безопасно пропускают.
//
// Здесь НЕТ Arduino/JSON-библиотек/heap: ручной сериализатор и парсер
// плоской структуры. Парсер толерантен к неизвестным ключам (has_pass,
// expiry_str из веб-API монолита), но строг к скелету: порча -> -1,
// и CardStore восстанавливается из бэкапа (никаких половинчатых баз).
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "SmartLockEvents.h"   // KeyType (тоже чистый заголовок)

// Рекорд базы (RAM-кэш CardStore). Размер фиксирован — никаких String.
struct SlUser {
    char     id[9];      // HEX карты, ВЕРХНИЙ регистр (монолит: toUpperCase)
    char     name[65];   // имя жильца (UTF-8, кавычки/бэкслеш запрещены)
    uint8_t  type;       // KeyType
    uint8_t  track;      // voice_track 0..99 (0 = без именной озвучки)
    uint32_t expiry;     // unix-секунды; 0 = бессрочно
    char     pin[7];     // веб-ПИН 4..6 цифр; "" = «Только карта»
    // --- Статистика и блокировка (5.0.x; идея AccessManager мёртвой ветки) --
    // Поля добавлены ПОСЛЕ pin: парсер v5.0.0 их пропустит (skipValue),
    // парсер монолита — тоже. Отсутствие в файле = нули (SlUser{}).
    uint32_t uses;       // счётчик проходов по карте
    uint32_t lastUse;    // unix последнего прохода; 0 = ни разу / время было
                         // недостоверно (UTC-бюджет: 4 байта до 2106 года)
    uint8_t  blocked;    // 1 = карта заблокирована (потеряна): отказ без
                         // удаления — имя/статистика сохраняются для разбора
};

namespace carddb {

// --- ТИПЫ КЛЮЧЕЙ -------------------------------------------------------------
inline const char* typeStr(uint8_t t) {
    switch ((KeyType)t) {
        case KeyType::MASTER:    return "master";
        case KeyType::TEMPORARY: return "temporary";
        case KeyType::ONETIME:   return "one-time";
        default:                 return "permanent";
    }
}

inline uint8_t typeFromStr(const char* s) {
    if (strcmp(s, "master")    == 0) return (uint8_t)KeyType::MASTER;
    if (strcmp(s, "temporary") == 0) return (uint8_t)KeyType::TEMPORARY;
    if (strcmp(s, "one-time")  == 0) return (uint8_t)KeyType::ONETIME;
    return (uint8_t)KeyType::PERMANENT;
}

// --- НОРМАЛИЗАЦИЯ ------------------------------------------------------------
/// ID карты: 4..8 HEX-символов, верхний регистр, ЛЕВЫЙ паддинг нулями до 8.
/// 5.1.1: раньше требовалось РОВНО 8 — но W26-считыватель публикует
/// %06lX (6 цифр, формат монолита), а панель принимает 4..8. Короткий
/// ввод с брелока ("898989") отвергался как bad_id, и оператор получал
/// ложное «такой HEX уже есть». Паддинг согласует ВСЕ пути разом
/// (find/add/remove/update проходят через эту функцию): карта, прочитанная
/// как "898989", и введённая руками как "00898989" — один ключ.
/// false — мусор (не добавлять в базу).
inline bool normalizeId(const char* in, char out[9]) {
    if (!in) return false;
    size_t n = strlen(in);
    if (n < 4 || n > 8) return false;
    size_t pad = 8 - n;
    for (size_t i = 0; i < pad; ++i) out[i] = '0';
    for (size_t i = 0; i < n; ++i) {
        char c = in[i];
        if (c >= '0' && c <= '9') { out[pad + i] = c; continue; }
        if (c >= 'a' && c <= 'f') c -= 32;
        if (c >= 'A' && c <= 'F') { out[pad + i] = c; continue; }
        return false;
    }
    out[8] = '\0';
    return true;
}

/// Имя: вырезаем кавычки/бэкслеш/управляющие (JSON-безопасность без
/// полноценного экранирования — имена задаёт администратор, не карта).
/// 5.2.0: '{' и '}' тоже под нож — потоковый загрузчик вырезает записи
/// из файла подсчётом скобок, скобки внутри строк разрушили бы грамматику
/// (экранирования в нашем JSON нет по построению, см. parseString).
inline void sanitizeName(const char* in, char* out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < outSize; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\' || c < 0x20 || c == '{' || c == '}') continue;
        out[o++] = (char)c;
    }
    if (outSize) out[o] = '\0';
}

/// Веб-ПИН жильца: либо "" (нет веб-доступа), либо строго 4..6 цифр
/// (монолит: регулярка ^\d{4,6}$ в веб-форме). false — мусор.
inline bool normalizePin(const char* in, char out[7]) {
    out[0] = '\0';
    if (!in || in[0] == '\0') return true;    // пусто — легально («только карта»)
    size_t n = strlen(in);
    if (n < 4 || n > 6) return false;
    for (size_t i = 0; i < n; ++i) {
        if (in[i] < '0' || in[i] > '9') return false;
        out[i] = in[i];
    }
    out[n] = '\0';
    return true;
}

// --- ПОИСК (5.1.2) -------------------------------------------------------------
// Регистронезависимое вхождение подстроки: ASCII + кириллица UTF-8.
// Свёртка: A-Z -> a-z; А..П (D0 90..9F) -> D0 b+0x20; Р..Я (D0 A0..AF) ->
// D1 b-0x20; Ё (D0 81) -> ё (D1 91). Прочие байты сравниваются как есть —
// равенство многобайтовых последовательностей сохраняется побайтово.
inline void foldCp(const char* p, uint8_t& consumed, uint8_t out[2]) {
    uint8_t c = (uint8_t)p[0];
    if (c >= 'A' && c <= 'Z') { consumed = 1; out[0] = c + 32; out[1] = 0; return; }
    // Кириллица живёт в лид-байтах D0/D1: ВСЕГДА отдаём полную пару
    // (свёрнутую или копию) — иначе двухбайтовый символ с одной стороны
    // сравнивался бы с однобайтовыми кусками с другой (рассинхрон).
    if (c == 0xD0) {
        uint8_t d = (uint8_t)p[1];
        consumed = 2;
        if (d >= 0x90 && d <= 0x9F) { out[0] = 0xD0; out[1] = d + 0x20; return; }
        if (d >= 0xA0 && d <= 0xAF) { out[0] = 0xD1; out[1] = d - 0x20; return; }
        if (d == 0x81)              { out[0] = 0xD1; out[1] = 0x91;    return; }
        out[0] = c; out[1] = d; return;
    }
    if (c == 0xD1) { consumed = 2; out[0] = c; out[1] = (uint8_t)p[1]; return; }
    consumed = 1; out[0] = c; out[1] = 0;
}

/// needle входит в haystack (регистр не важен)? Пустой needle = истина.
inline bool containsCI(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    if (!*needle) return true;
    for (const char* h = hay; *h; ++h) {
        const char* hp = h;
        const char* np = needle;
        for (;;) {
            if (!*np) return true;
            if (!*hp) break;
            uint8_t hc, nc, ho[2], no[2];
            foldCp(hp, hc, ho);
            foldCp(np, nc, no);
            if (ho[0] != no[0] || ho[1] != no[1]) break;
            hp += hc;
            np += nc;
        }
    }
    return false;
}

// --- СЕРИАЛИЗАЦИЯ -------------------------------------------------------------
/// ОДНА запись {"id":...} в buf (first=false — с ведущей запятой).
/// 5.2.0: выделена из serialize — потоковый save пишет базу 250 карт
/// по записи (стековый кусок ~320 Б), без монструозного буфера на всю базу.
/// Возвращает длину или 0 (не влезло).
inline size_t serializeOne(const SlUser& u, bool first, char* buf, size_t bufSize) {
    // Запись БЕЗ закрывающей '}': хвост (pin, статистика) дописывается
    // ниже. pin пишем только когда задан — формат монолита толерантен,
    // а «Только карта» не засоряет файл (и не светит пустышки).
    int n = snprintf(buf, bufSize,
        "%s{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\","
        "\"track\":%u,\"expiry\":%lu%s%s%s",
        first ? "" : ",", u.id, u.name, typeStr(u.type),
        (unsigned)u.track, (unsigned long)u.expiry,
        u.pin[0] ? ",\"pin\":\"" : "",
        u.pin[0] ? u.pin : "",
        u.pin[0] ? "\"" : "");
    if (n < 0 || (size_t)n >= bufSize) return 0;
    size_t pos = (size_t)n;
    // Статистика/блокировка — тоже только не-умолчания: новая база и база
    // монолита остаются байт-в-байт прежними, нули файл не засоряют
    if (u.uses > 0) {
        n = snprintf(buf + pos, bufSize - pos, ",\"uses\":%lu",
                     (unsigned long)u.uses);
        if (n < 0 || (size_t)n >= bufSize - pos) return 0;
        pos += (size_t)n;
    }
    if (u.lastUse > 0) {
        n = snprintf(buf + pos, bufSize - pos, ",\"last_use\":%lu",
                     (unsigned long)u.lastUse);
        if (n < 0 || (size_t)n >= bufSize - pos) return 0;
        pos += (size_t)n;
    }
    if (u.blocked) {
        n = snprintf(buf + pos, bufSize - pos, ",\"blocked\":1");
        if (n < 0 || (size_t)n >= bufSize - pos) return 0;
        pos += (size_t)n;
    }
    if (pos + 2 >= bufSize) return 0;    // место под '}' и '\0'
    buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}

/// {"users":[...]} в buf. Возвращает длину или 0 (не влезло — буфер мал:
/// вызывающий обязан заложить ~180 байт на запись с ПИНом и статистикой).
inline size_t serialize(const SlUser* users, uint8_t count,
                        char* buf, size_t bufSize) {
    size_t pos = 0;
    int n = snprintf(buf, bufSize, "{\"users\":[");
    if (n < 0 || (size_t)n >= bufSize) return 0;
    pos = (size_t)n;
    for (uint8_t i = 0; i < count; ++i) {
        size_t w = serializeOne(users[i], i == 0, buf + pos, bufSize - pos);
        if (w == 0) return 0;              // откат: не пишем кашу
        pos += w;
    }
    n = snprintf(buf + pos, bufSize - pos, "]}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + (size_t)n;
}

// --- ПАРСЕР (строгий скелет, толерантные ключи) --------------------------------
// Грамматика минимальна: {"users":[ { ... }, ... ]}. Любое отклонение — -1.
// Неизвестные пары "ключ":значение пропускаются (совместимость вперёд/назад).

inline const char* skipWs(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/// Строка в кавычках -> out (без экранирования: наш сериализатор его не
/// создаёт; чужие \uXXXX — не наш формат, отвергаем как порчу).
/// Возвращает указатель ПОСЛЕ закрывающей кавычки или nullptr.
inline const char* parseString(const char* p, char* out, size_t outSize) {
    if (*p != '"') return nullptr;
    ++p;
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\') return nullptr;      // экранирования в нашем JSON нет
        if (o + 1 < outSize) out[o++] = *p;
        ++p;
    }
    if (*p != '"') return nullptr;
    if (outSize) out[o] = '\0';
    return p + 1;
}

/// Число (только неотрицательные целые — track/expiry).
inline const char* parseUint(const char* p, uint32_t& out) {
    if (*p < '0' || *p > '9') return nullptr;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFUL - d) / 10) return nullptr;   // переполнение
        v = v * 10 + d;
        ++p;
    }
    out = v;
    return p;
}

/// Пропустить значение неизвестного ключа: строка, число, true/false/null.
inline const char* skipValue(const char* p) {
    p = skipWs(p);
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') { if (*p == '\\') ++p; ++p; }
        return (*p == '"') ? p + 1 : nullptr;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        ++p;
        while (*p >= '0' && *p <= '9') ++p;
        return p;
    }
    if (strncmp(p, "true", 4) == 0)  return p + 4;
    if (strncmp(p, "false", 5) == 0) return p + 5;
    if (strncmp(p, "null", 4) == 0)  return p + 4;
    return nullptr;
}

/// Разбор ОДНОГО объекта записи: p — на '{', по выходу p — ПОСЛЕ '}'.
/// 0 — ок (dst заполнен), -1 — порча. 5.2.0: выделен из parse — потоковый
/// загрузчик кормит записи по одной (база 250 карт не читается целиком
/// в RAM); parse() и parseOne() используют этот же разбор — источник один.
inline int parseObject(const char*& p, SlUser& dst) {
    if (*p != '{') return -1;
    p = skipWs(p + 1);

    SlUser u{};   // нули: отсутствующие поля = умолчания
    u.type = (uint8_t)KeyType::PERMANENT;
    char key[24];

    // Поля объекта (порядок не гарантирован — толерантность)
    for (;;) {
        p = parseString(p, key, sizeof(key));
        if (!p) return -1;
        p = skipWs(p);
        if (*p != ':') return -1;
        p = skipWs(p + 1);

        if (strcmp(key, "id") == 0) {
            p = parseString(p, u.id, sizeof(u.id));
        } else if (strcmp(key, "name") == 0) {
            p = parseString(p, u.name, sizeof(u.name));
        } else if (strcmp(key, "type") == 0 || strcmp(key, "key_type") == 0) {
            char t[12];
            p = parseString(p, t, sizeof(t));
            if (p) u.type = typeFromStr(t);
        } else if (strcmp(key, "track") == 0 ||
                   strcmp(key, "voice_track") == 0) {
            uint32_t v;
            p = parseUint(p, v);
            if (p) u.track = (v > 99) ? 99 : (uint8_t)v;
        } else if (strcmp(key, "expiry") == 0) {
            uint32_t v;
            p = parseUint(p, v);
            if (p) u.expiry = v;
        } else if (strcmp(key, "pin") == 0 ||
                   strcmp(key, "user_pin") == 0) {
            p = parseString(p, u.pin, sizeof(u.pin));
        } else if (strcmp(key, "uses") == 0) {
            p = parseUint(p, u.uses);
        } else if (strcmp(key, "last_use") == 0) {
            p = parseUint(p, u.lastUse);
        } else if (strcmp(key, "blocked") == 0) {
            uint32_t v;
            p = parseUint(p, v);
            if (p) u.blocked = v ? 1 : 0;
        } else {
            p = skipValue(p);   // has_pass, expiry_str, будущие поля
        }
        if (!p) return -1;
        p = skipWs(p);
        if (*p == ',') { p = skipWs(p + 1); continue; }
        if (*p == '}') { p = skipWs(p + 1); break; }
        return -1;
    }

    // ID обязателен и нормализуется; битый ID = порча записи = -1
    if (!normalizeId(u.id, dst.id)) return -1;
    sanitizeName(u.name, dst.name, sizeof(dst.name));
    dst.type    = u.type;
    dst.track   = u.track;
    dst.expiry  = u.expiry;
    dst.uses    = u.uses;
    dst.lastUse = u.lastUse;
    dst.blocked = u.blocked;
    // ПИН: битый (не 4..6 цифр) — не порча файла, а лишение веб-доступа
    // (редактировали руками — наказуем запись, а не всю базу)
    if (!normalizePin(u.pin, dst.pin)) dst.pin[0] = '\0';
    return 0;
}

/// Одна запись как отдельная строка "{...}" (потоковый загрузчик 5.2.0).
/// 0 — ок, -1 — порча/мусор вокруг скобок.
inline int parseOne(const char* obj, SlUser& dst) {
    if (!obj) return -1;
    const char* p = skipWs(obj);
    if (parseObject(p, dst) != 0) return -1;
    p = skipWs(p);
    return (*p == '\0') ? 0 : -1;
}

/// Разбор всего файла. Возвращает число записей (0..maxCount) или -1
/// (порча формата — вызывающий восстанавливает из бэкапа).
inline int parse(const char* json, SlUser* out, uint8_t maxCount) {
    if (!json) return -1;
    const char* p = skipWs(json);
    if (*p != '{') return -1;
    p = skipWs(p + 1);

    char key[24];
    p = parseString(p, key, sizeof(key));
    if (!p || strcmp(key, "users") != 0) return -1;
    p = skipWs(p);
    if (*p != ':') return -1;
    p = skipWs(p + 1);
    if (*p != '[') return -1;
    p = skipWs(p + 1);

    uint8_t count = 0;
    if (*p == ']') {  // пустая база — легально
        p = skipWs(p + 1);
        return (*p == '}') ? 0 : -1;
    }

    for (;;) {
        if (count >= maxCount) return -1;   // база больше RAM-кэша — порча
        if (parseObject(p, out[count]) != 0) return -1;
        ++count;

        if (*p == ',') { p = skipWs(p + 1); continue; }
        if (*p == ']') { p = skipWs(p + 1); break; }
        return -1;
    }
    return (*p == '}') ? (int)count : -1;
}

} // namespace carddb
