// ============================================================================
// MqttDescriptor.h — M3.4-Info: чистый контракт самоописания устройства
// ============================================================================
// Только формирование топика и JSON в заранее выделенный буфер. Без Arduino,
// heap, сети и конфигурации — поэтому вся логика проверяется host-тестом.
// Возврат 0 означает ошибку/нехватку буфера; усечённый JSON наружу не уходит.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace mqtt_descriptor {

constexpr uint8_t SCHEMA_VERSION = 1;

struct Fields {
    const char* id;
    const char* profile;
    const char* kernel;
    const char* profileVer;
    const char* host;
    const char* ip;
};

inline bool validId(const char* id) {
    if (id == nullptr) return false;
    for (uint8_t i = 0; i < 12; ++i) {
        const char c = id[i];
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;   // включая преждевременный '\0'
    }
    return id[12] == '\0';
}

struct Cursor {
    char* out;
    size_t cap;
    size_t len = 0;
    bool ok = true;
};

inline void put(Cursor& c, char ch) {
    if (!c.ok) return;
    if (c.out == nullptr || c.len + 1 >= c.cap) {  // место для '\0' — всегда
        c.ok = false;
        return;
    }
    c.out[c.len++] = ch;
}

inline void raw(Cursor& c, const char* s) {
    if (s == nullptr) { c.ok = false; return; }
    while (*s != '\0') put(c, *s++);
}

inline void jsonString(Cursor& c, const char* s) {
    if (s == nullptr || s[0] == '\0') { c.ok = false; return; }
    static const char HEX_DIGITS[] = "0123456789abcdef";
    put(c, '"');
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        const unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            put(c, '\\');
            put(c, (char)ch);
        } else if (ch < 0x20) {
            raw(c, "\\u00");
            put(c, HEX_DIGITS[(ch >> 4) & 0x0F]);
            put(c, HEX_DIGITS[ch & 0x0F]);
        } else {
            put(c, (char)ch);       // UTF-8 байты проходят без перекодировки
        }
    }
    put(c, '"');
}

inline size_t finish(Cursor& c) {
    if (!c.ok) {
        if (c.out != nullptr && c.cap > 0) c.out[0] = '\0';
        return 0;
    }
    if (c.out == nullptr || c.cap == 0) return 0;
    c.out[c.len] = '\0';
    return c.len;
}

/// <prefix>/<deviceId>/descriptor. Префикс не может содержать wildcard.
inline size_t buildTopic(char* out, size_t cap,
                         const char* prefix, const char* id) {
    Cursor c{out, cap};
    if (prefix == nullptr || prefix[0] == '\0' || prefix[0] == '/' ||
        !validId(id)) {
        return finish(c);
    }
    for (const char* p = prefix; *p != '\0'; ++p) {
        if (*p == '+' || *p == '#') return finish(c);
    }
    raw(c, prefix);
    if (c.len > 0 && c.out[c.len - 1] == '/') {
        c.ok = false;                 // семантический отказ тоже => 0 + ""
        return finish(c);
    }
    raw(c, "/");
    raw(c, id);
    raw(c, "/descriptor");
    return finish(c);
}

/// JSON схемы v1. Все поля обязательны; отсутствующее значение заранее
/// заменяется вызывающим на строку "unknown" — пустое поле здесь не принимаем.
inline size_t buildJson(char* out, size_t cap, const Fields& f) {
    Cursor c{out, cap};
    if (!validId(f.id)) return finish(c);
    raw(c, "{\"v\":1,\"id\":");
    jsonString(c, f.id);
    raw(c, ",\"profile\":");
    jsonString(c, f.profile);
    raw(c, ",\"kernel\":");
    jsonString(c, f.kernel);
    raw(c, ",\"profile_ver\":");
    jsonString(c, f.profileVer);
    raw(c, ",\"host\":");
    jsonString(c, f.host);
    raw(c, ",\"ip\":");
    jsonString(c, f.ip);
    put(c, '}');
    return finish(c);
}

} // namespace mqtt_descriptor
