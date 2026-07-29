const CACHE_NAME = 'scud-v2.5.0';
const ASSETS = ['/', '/index.html', '/manifest.json', '/icon-256.png', '/sha256.js']; // Добавили хешер в кэш

self.addEventListener('install', function(event) {
    event.waitUntil(
        caches.open(CACHE_NAME).then(function(cache) {
            return cache.addAll(ASSETS);
        })
    );
    self.skipWaiting(); // Принудительная активация новой версии воркера
});

self.addEventListener('activate', function(event) {
    event.waitUntil(self.clients.claim()); // Мгновенный перехват управления страницами
});

// --- БЕЗОПАСНЫЙ И ИНТЕЛЛЕКТУАЛЬНЫЙ ФИЛЬТР ЗАПРОСОВ СКУД ---
self.addEventListener('fetch', function(event) {
    // Если запрос идет к динамическим эндпоинтам бэкенда /api —
    // пускаем его СТРОГО в сеть напрямую, полностью минуя кэш смартфона!
    if (event.request.url.includes('/api/')) {
        event.respondWith(fetch(event.request));
        return;
    }

    // Для статических файлов (HTML, стили, иконка) используем быструю отдачу из кэша
    event.respondWith(
        caches.match(event.request).then(function(response) {
            return response || fetch(event.request);
        })
    );
});
