// Runs Ghostscript (WASM) off the main thread. Rendering is synchronous and
// can take a while, so it must not block the page.
import { loadGhostscript, runGhostscript } from '../dist/ghostscript.mjs';

loadGhostscript().then(
  () => postMessage({ type: 'ready' }),
  (e) => postMessage({ type: 'fatal', message: String(e) }),
);

const pageNumber = (name) => Number(name.match(/\d+/)[0]);

onmessage = async ({ data: { id, name, bytes, args } }) => {
  const log = [];
  const t0 = performance.now();
  try {
    const r = await runGhostscript([...args, name], {
      files: { [name]: bytes },
      onStdout: (l) => log.push(l),
      onStderr: (l) => log.push(l),
    });
    const decoder = new TextDecoder();
    const pages = Object.keys(r.files)
      .filter((f) => /^page-\d+\.svg$/.test(f))
      .sort((a, b) => pageNumber(a) - pageNumber(b))
      .map((f) => decoder.decode(r.files[f]));
    postMessage({ type: 'result', id, exitCode: r.exitCode, pages, log: log.join('\n'), ms: performance.now() - t0 });
  } catch (e) {
    log.push(String(e && e.stack || e));
    postMessage({ type: 'result', id, exitCode: -1, pages: [], log: log.join('\n'), ms: performance.now() - t0 });
  }
};
