// Open Fodder service worker: keeps the game on the device after the first visit, so it starts
// at once and plays offline. install.cmake stamps BUILD with a hash of the files below, so each
// build gets its own cache. A new build installs in the background and waits: it takes over only
// once no page of the old build is open, so a running page never mixes two builds' files.
const BUILD = '@OPENFODDER_BUILD@';
const CACHE = 'openfodder-' + BUILD;
const FILES = [
  './', 'index.html', 'OpenFodder.js', 'OpenFodder.wasm', 'OpenFodder.data',
  'manifest.webmanifest', 'icon-180.png', 'icon-192.png', 'icon-512.png', 'icon-maskable-512.png',
];

self.addEventListener('install', event => {
  // no-cache revalidates against the server (a 304 when the page just downloaded the file),
  // so an HTTP-cached file from an older build never lands in this build's cache
  event.waitUntil(
    caches.open(CACHE)
      .then(cache => cache.addAll(FILES.map(file => new Request(file, { cache: 'no-cache' }))))
  );
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys
        .filter(key => key.startsWith('openfodder-') && key !== CACHE)
        .map(key => caches.delete(key))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', event => {
  const request = event.request;
  if (request.method !== 'GET' || new URL(request.url).origin !== self.location.origin)
    return;

  event.respondWith(
    caches.open(CACHE)
      .then(cache => cache.match(request, { ignoreSearch: true }))
      .then(hit => hit || fetch(request))
  );
});
