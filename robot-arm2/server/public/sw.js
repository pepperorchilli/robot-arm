// Service Worker：让网页可离线/快速打开
//
// 2026-09 修正缓存策略
//
// 原来用的是「缓存优先」：
//     caches.match(req).then(r => r || fetch(req))
// 一旦某个页面被缓存，浏览器就**永远**拿旧版本，服务器更新了也看不到。
// 实际踩到：首页改版后刷新多次仍显示旧页面。
//
// 改为「网络优先」：先请求网络，拿到就用新的并顺手更新缓存；
// 只有离线（请求失败）时才回退到缓存。
// 对个人站点来说，这点网络开销远小于"改了看不到"的困扰。

// 版本号一改，旧缓存在 activate 时会被清掉（用户的旧缓存就是这么清掉的）
const CACHE = 'robot-arm-v3';

self.addEventListener('install', () => {
  // 不做预缓存：网络优先策略下，资源会在首次访问时自然进缓存。
  // 预缓存反而会拖慢安装，还可能因为个别资源 404 导致整个安装失败。
  self.skipWaiting();
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(
        keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))
      ))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);

  // 只处理同源的 GET
  if (e.request.method !== 'GET' || url.origin !== location.origin) return;

  // 接口类请求不缓存，直接走网络（否则会拿到过期数据）
  if (url.pathname.startsWith('/api/') ||
      url.pathname.startsWith('/set') ||
      url.pathname.startsWith('/library/api/') ||
      url.pathname.startsWith('/ws')) {
    return;
  }

  // 网络优先：拿到就用新的，顺便更新缓存；失败才回退到缓存（离线可看）
  e.respondWith(
    fetch(e.request)
      .then((res) => {
        if (res && res.status === 200 && res.type === 'basic') {
          const copy = res.clone();
          caches.open(CACHE).then((c) => c.put(e.request, copy)).catch(() => {});
        }
        return res;
      })
      .catch(() => caches.match(e.request))
  );
});
