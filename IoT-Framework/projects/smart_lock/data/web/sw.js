// ============================================================================
// sw.js — Service Worker панели МикроОС 5.0 (адаптация sw.js монолита v2.5.0)
// Статика — из кэша (быстрый запуск «приложения» с домашнего экрана),
// /api — СТРОГО в сеть: динамику СКУД кэшировать нельзя никогда.
// ============================================================================
const CACHE_NAME = 'microos-lock-v5.2.0';
const ASSETS = [
  '/web/lock.html',
  '/web/manifest.json',
  '/web/voice_map.json',
  '/web/uplot.js',
  '/web/uplot.css',
  '/web/icon-256.png',
  '/web/icon.png',
  '/web/favicon.png'
];

self.addEventListener('install', function(event) {
  event.waitUntil(
    caches.open(CACHE_NAME).then(function(cache) { return cache.addAll(ASSETS); })
  );
  self.skipWaiting(); // принудительная активация новой версии воркера
});

self.addEventListener('activate', function(event) {
  event.waitUntil(
    // Сносим кэши старых версий, затем мгновенно перехватываем страницы
    caches.keys().then(function(keys) {
      return Promise.all(keys.map(function(k) {
        if (k !== CACHE_NAME) return caches.delete(k);
      }));
    }).then(function() { return self.clients.claim(); })
  );
});

// --- БЕЗОПАСНЫЙ ФИЛЬТР ЗАПРОСОВ СКУД (семантика монолита сохранена) ---
self.addEventListener('fetch', function(event) {
  // Динамические эндпоинты бэкенда — только сеть, минуя кэш смартфона
  if (event.request.url.indexOf('/api/') >= 0) {
    event.respondWith(fetch(event.request));
    return;
  }
  // Статика: сначала кэш, промах — сеть (и пополнение кэша)
  event.respondWith(
    caches.match(event.request).then(function(cached) {
      return cached || fetch(event.request);
    })
  );
});
