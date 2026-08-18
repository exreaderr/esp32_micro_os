#ifndef SMQTTPLATFORM_FILE
#define SMQTTPLATFORM_FILE

#define SMQTT_DEPRECATED(msg) [[deprecated(msg)]]

#if (__cplusplus >= 201703L)
#define MAYBE_UNUSED [[maybe_unused]]
#else
#define MAYBE_UNUSED
#endif

#if defined(SMQTT_USER_SOCKET)
// create and write your client and server
#include"smqtt_user_socket.h"
#elif defined(WIN32)
class TCPClient {
public:
	bool available() { return false; };
	char read() { return 0; }
	bool connected() {
		return false;
	}
	void stop() {}
	void write(const char *, int) {}
};
class TCPServer {
public:
	TCPServer(short) {}
	void begin() {}
};
#define SMQTT_LOGD
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#define TCPClient WiFiClient
#define TCPServer WiFiServer
#if defined(DEBUG_ESP_PORT)
#define SMQTT_LOGD(...) DEBUG_ESP_PORT.printf(__VA_ARGS__)
#else
#define SMQTT_LOGD(...)
#endif
#elif defined(ESP32)
// --- Патч МикроОС (M1, 06.08.2026) -------------------------------------
// Параноидальный проводной контур: брокер обязан НЕ тянуть WiFi-стек в
// прошивку. WiFi.h здесь — только ради псевдонимов WiFiClient/WiFiServer
// (в ядре 3.x это NetworkClient/NetworkServer — сокеты lwIP, работают
// поверх Ethernet/W5500 без инициализации радио). Все обращения к
// глобальному объекту WiFi (WiFi.isConnected и т.п.) вычищены из кода
// ниже по дереву под макросом SMQTT_WIRED_ONLY: одна ссылка на WiFiClass
// — и линкер утянет esp_wifi (+337 КБ, измерено на сборке 5.3.2).
#define SMQTT_WIRED_ONLY 1
#include <WiFi.h>
#ifdef SMQTT_WT32_ETH01
#include <ETH.h>
#endif
#define TCPClient WiFiClient
#define TCPServer WiFiServer
MAYBE_UNUSED static const char *SMQTTTAG = "sMQTTBroker";
#define SMQTT_LOGD(...) ESP_LOGD(SMQTTTAG,__VA_ARGS__)
#elif defined(WIO_TERMINAL)
#include <rpcWiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#define TCPClient WiFiClient
#define TCPServer WiFiServer
#define SMQTT_LOGD(...)
#else
#error "unknown platform"
#endif

#endif