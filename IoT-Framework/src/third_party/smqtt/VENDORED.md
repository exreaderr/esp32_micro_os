# Vendored: sMQTTBroker (этап M1, spike встроенного брокера)

Источник: https://github.com/terrorsl/sMQTTBroker, ветка `main`,
снимок от 24.06.2026 (последний пуш в репозиторий).
Лицензия: MIT (файл LICENSE рядом, сохранён без изменений).

## Зачем вендоринг, а не внешняя зависимость
Единый самодостаточный комплект МикроОС: библиотека собирается вместе
с ядром из `src/third_party`, версия зафиксирована, внешних скачиваний
при сборке нет. Компилируется только там, где её кто-то использует
(объектники попадают в бинарь по требованию линкера — smart_lock и
прочие профили без брокера не платят ни байтом flash, ни байтом RAM).

## Патчи МикроОС поверх апстрима (ровно три, все помечены в коде)
1. `sMQTTplatform.h` — ветка ESP32: добавлен `#define SMQTT_WIRED_ONLY 1`
   с пояснением. Сам по себе `#include <WiFi.h>` оставлен: он даёт лишь
   псевдонимы WiFiClient/WiFiServer (= NetworkClient/NetworkServer ядра
   3.x, сокеты lwIP поверх Ethernet/W5500, радио не инициализируют).
2. `sMQTTBroker.cpp::update()` — блок проверки `WiFi.isConnected()`
   обёрнут в `#ifndef SMQTT_WIRED_ONLY`: ссылка на глобальный объект
   WiFiClass утянула бы в прошивку весь WiFi-стек (+337 КБ, измерено
   на сборке 5.3.2 при попытке «явно выключить радио»).
3. `sMQTTEvent.h` + `sMQTTBroker.cpp::publish(client,topic,msg)` (M2):
   `sMQTTPublicClientEvent` дополнен retain-флагом исходного PUBLISH
   (`setRetain`/`Retain`, источник — `msg->isRetained()`). Мост МикроОС
   обязан транслировать retained-состояния устройств на вышестоящий
   брокер «как есть»; без флага в событии это было невозможно.

## Известные ограничения движка (для spike-протокола M1)
- MQTT 3.1.1, QoS 0/1 (PUBACK есть; DUP/QoS2/MQTT5 — нет).
- Retained: ПОЛНОЦЕННО (хранение, выдача новым подписчикам, очистка
  пустым payload, матчинг по wildcard — sMQTTTopic::match).
- LWT (Last Will): парсится из CONNECT, но НЕ исполняется (строки
  willTopic/willMessage закомментированы в апстриме). Для контура HA
  компенсируется retained-heartbeat'ами устройств (наш стандарт).
- Аутентификация: user/password через событие NewClient (реализовано
  в BrokerService; отказ = CONNACK 0x04 + закрытие сокета).
- Память: `new sMQTTClient` на подключение + std::string/std::vector —
  всё во внутренней RAM. Замер стоимости одного подключения и потолка
  в 32 клиента — главная задача spike'а (хуки в BrokerService).
- Потолка клиентов в апстриме нет — ограничение вводит BrokerService
  через отказ в NewClient.
