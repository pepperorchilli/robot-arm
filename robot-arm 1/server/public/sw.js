// Service Worker：缓存静态资源，让网页可离线/快速打开
const CACHE = 'robot-arm-v1';
const ASSETS = [
  '/',
  '/control',
  '/messages',
  '/download',
  '/style.css',
  '/app.js',
  '/icon-192.png',
  '/icon-512.png'
];

self.addEventListener('install', (e) => {
  e.waitUntil(
    caches.open(CACHE).then((c) => c.addAll(ASSETS)).then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (e) => {
  // 只缓存 GET；API(/api, /set) 没有缓存时会直接走网络
  e.respondWith(
    caches.match(e.request).then((r) => r || fetch(e.request))
  );
});
