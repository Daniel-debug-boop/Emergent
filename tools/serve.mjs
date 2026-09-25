/**
 * EMERGENT static file server.
 *
 * The game is a dependency-free static site: this server exists so the project
 * can be previewed and smoke-tested without a Python runtime, and it binds
 * 0.0.0.0 so the managed preview can reach it.
 *
 * Usage: `node tools/serve.mjs` (honours PORT, default 8765)
 */
import http from 'node:http';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..'));
const PORT = Number(process.env.PORT || 8765);
const HOST = '0.0.0.0';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.glsl': 'text/plain; charset=utf-8'
};

/**
 * Resolve a request path to a file inside ROOT, or null when it escapes the
 * root or does not exist. Rejecting traversal here is the only access control
 * this server has, so it must be correct rather than best-effort.
 */
async function resolveFile(urlPath) {
  const decoded = decodeURIComponent(urlPath.split('?')[0]);
  const relative = decoded === '/' ? 'index.html' : decoded.replace(/^\/+/, '');
  const target = path.resolve(ROOT, relative);
  if (target !== ROOT && !target.startsWith(ROOT + path.sep)) return null;
  try {
    const info = await stat(target);
    if (info.isDirectory()) {
      const index = path.join(target, 'index.html');
      await stat(index);
      return index;
    }
    return target;
  } catch {
    return null;
  }
}

const server = http.createServer(async (req, res) => {
  const file = await resolveFile(req.url || '/');
  if (!file) {
    res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
    res.end('404 Not Found');
    return;
  }
  res.writeHead(200, {
    'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
    'cache-control': 'no-cache'
  });
  createReadStream(file).pipe(res);
});

/** Start the server on the managed preview port. Exported for tests. */
export function start(port = PORT, host = HOST) {
  return new Promise(resolve => {
    server.listen(port, host, () => {
      // Report the port actually bound, which matters when 0 asks the OS to pick.
      const bound = server.address().port;
      process.stdout.write(`EMERGENT serving ${ROOT} on http://${host}:${bound}\n`);
      resolve(server);
    });
  });
}

export { resolveFile, ROOT, MIME };

// Only listen when executed directly, so tests can import the path logic
// without binding a port.
if (process.argv[1] && import.meta.url === new URL(`file://${process.argv[1]}`).href) {
  await start();
  for (const signal of ['SIGINT', 'SIGTERM']) {
    process.on(signal, () => server.close(() => process.exit(0)));
  }
}
