// Ghostscript -> SVG demo. Ghostscript runs in a worker (worker.js); each
// page comes back as a standalone SVG document that is inlined here, so it
// can be zoomed, its text selected and searched, and downloaded.

const EX = '../src/ghostscript-10.08.0/examples/';
const SAMPLES = [
  ['Hello, SVG', 'samples/hello.ps'],
  ['Tiger (EPS)', EX + 'tiger.eps'],
  ['Golfer (EPS)', EX + 'golfer.eps'],
  ['Escher', EX + 'escher.ps'],
  ['Colour wheel', EX + 'colorcir.ps'],
  ['Font waterfall', EX + 'waterfal.ps'],
  ['Shadings → gradients', 'samples/shadings.ps'],
  ['Strokes: caps, joins, dashes', 'samples/strokes.ps'],
  ['Images', 'samples/images.ps'],
  ['Transparency (PDF)', 'samples/transparency.pdf'],
  ['Text, graphics & image (PDF)', EX + 'text_graphic_image.pdf'],
  ['Annotations (PDF, 6 pages)', EX + 'annots.pdf'],
];

const $ = (id) => document.getElementById(id);
const editor = $('editor');
const pagesEl = $('pages');

// PostScript is bytes, not text: map bytes to chars 1:1 so binary data in
// a file survives a round trip through the editor.
const bytesToText = (b) => {
  let s = '';
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
  return s;
};
const textToBytes = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0) & 0xff);

let source = null;      // { name, bytes, binary }
let edited = false;

// ---- Worker ---------------------------------------------------------------

let worker, ready = false, running = null, queued = null, seq = 0;

function startWorker() {
  worker = new Worker('worker.js', { type: 'module' });
  ready = false;
  setStatus('Loading Ghostscript…', 'busy');
  worker.onmessage = ({ data }) => {
    if (data.type === 'ready') {
      ready = true;
      setStatus('Ready', 'ready');
      next();
    } else if (data.type === 'fatal') {
      setStatus('Failed to load Ghostscript', 'error');
      showLog(data.message, true);
    } else if (data.type === 'result' && running && data.id === running.id) {
      running = null;
      show(data);
      next();
    }
  };
  worker.onerror = (e) => {
    setStatus('Worker error', 'error');
    showLog(e.message || String(e), true);
  };
}

function requestRender() {
  if (!source) return;
  const name = source.name;
  const bytes = edited ? withShowpage(textToBytes(editor.value)) : source.bytes;
  const args = ['-sDEVICE=svg', '-o', 'page-%d.svg',
    `-dSVGTextLayer=${$('text-layer').checked}`, `-dSVGReuseGlyphs=${$('reuse').checked}`,
    `-dSVGTransparency=${$('transparency').checked}`];
  if (/\.(eps|epsf|ai)$/i.test(name)) args.push('-dEPSCrop');
  queued = { id: ++seq, name, bytes, args };
  next();
}

function next() {
  if (!ready || running || !queued) return;
  running = queued;
  queued = null;
  setStatus('Rendering…', 'busy');
  // Copy: the bytes are transferred to the worker.
  const bytes = running.bytes.slice();
  worker.postMessage({ id: running.id, name: running.name, bytes, args: running.args }, [bytes.buffer]);
}

// Edited PostScript often lacks a final showpage; add one so there's output.
function withShowpage(bytes) {
  const text = bytesToText(bytes);
  return /showpage\s*$/.test(text.trimEnd()) || /%PDF-/.test(text.slice(0, 1024))
    ? bytes : textToBytes(text + '\nshowpage\n');
}

// ---- Output ---------------------------------------------------------------

let urls = [];

function show({ exitCode, pages, log, ms }) {
  urls.forEach(URL.revokeObjectURL);
  urls = [];
  pagesEl.replaceChildren();
  const failed = exitCode !== 0;
  showLog(log, failed || pages.length === 0);
  if (failed) {
    const banner = document.createElement('div');
    banner.className = 'error-banner';
    banner.textContent = `Ghostscript exited with code ${exitCode}. See the output on the left.`;
    pagesEl.append(banner);
  }
  if (!pages.length) {
    if (!failed) pagesEl.insertAdjacentHTML('beforeend', '<p class="empty">No pages were produced.</p>');
    setStatus(failed ? 'Error' : 'No output', failed ? 'error' : 'ready');
    $('stats').textContent = '';
    return;
  }
  let total = 0;
  pages.forEach((svgText, i) => {
    total += svgText.length;
    pagesEl.append(pageView(svgText, i + 1));
  });
  applyZoom();
  const kb = (n) => (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
  $('stats').textContent = `${pages.length} page${pages.length > 1 ? 's' : ''} · ${kb(total)} · ${Math.round(ms)} ms`;
  setStatus(failed ? 'Error' : `Rendered in ${Math.round(ms)} ms`, failed ? 'error' : 'ready');
}

function pageView(svgText, n) {
  const doc = new DOMParser().parseFromString(svgText, 'image/svg+xml');
  const svg = doc.documentElement;
  const wrap = document.createElement('div');
  wrap.className = 'page';
  const head = document.createElement('div');
  head.className = 'page-head';
  if (svg.nodeName !== 'svg') {
    head.textContent = `Page ${n}: invalid SVG`;
    wrap.append(head);
    return wrap;
  }
  prefixIds(svg, `p${n}-`);
  const count = (sel) => svg.querySelectorAll(sel).length;
  const w = parseFloat(svg.getAttribute('width')), h = parseFloat(svg.getAttribute('height'));
  const url = URL.createObjectURL(new Blob([svgText], { type: 'image/svg+xml' }));
  urls.push(url);
  head.innerHTML = `<strong>Page ${n}</strong><span>${w} × ${h} pt · ${(svgText.length / 1024).toFixed(1)} KB · ` +
    `${count('use')} glyphs · ${count('image')} images · ${count('linearGradient, radialGradient')} gradients</span>` +
    `<a href="${url}" download="page-${n}.svg">Download</a>`;
  const sheet = document.createElement('div');
  sheet.className = 'sheet';
  const node = document.importNode(svg, true);
  node.dataset.w = w;
  node.dataset.h = h;
  sheet.append(node);
  wrap.append(head, sheet);
  return wrap;
}

// Each page's ids (g1, c1, s1, ...) are only unique within that page.
// Prefix them so several pages can live in one document.
function prefixIds(svg, prefix) {
  const ids = new Set();
  svg.querySelectorAll('[id]').forEach((el) => {
    ids.add(el.id);
    el.id = prefix + el.id;
  });
  if (!ids.size) return;
  const XLINK = 'http://www.w3.org/1999/xlink';
  svg.querySelectorAll('*').forEach((el) => {
    for (const attr of [...el.attributes]) {
      if ((attr.localName === 'href') && attr.value.startsWith('#') && ids.has(attr.value.slice(1))) {
        attr.namespaceURI === XLINK
          ? el.setAttributeNS(XLINK, 'xlink:href', '#' + prefix + attr.value.slice(1))
          : el.setAttribute('href', '#' + prefix + attr.value.slice(1));
      } else if (attr.value.includes('url(#')) {
        el.setAttribute(attr.name, attr.value.replace(/url\(#([^)]+)\)/g,
          (m, id) => (ids.has(id) ? `url(#${prefix}${id})` : m)));
      }
    }
  });
}

function applyZoom() {
  const z = Number($('zoom').value);
  pagesEl.querySelectorAll('.sheet > svg').forEach((svg) => {
    // 1pt = 4/3 CSS px
    svg.style.width = `${(svg.dataset.w * 4 / 3) * z}px`;
    svg.style.height = `${(svg.dataset.h * 4 / 3) * z}px`;
  });
}

function showLog(text, open) {
  $('log').textContent = text || '(no output)';
  if (open) $('log-box').open = true;
}

function setStatus(text, cls) {
  const el = $('status');
  el.textContent = text;
  el.className = 'status ' + (cls || '');
}

// ---- Sources --------------------------------------------------------------

function setSource(name, bytes) {
  const binary = /\.pdf$/i.test(name) || bytesToText(bytes.subarray(0, 1024)).startsWith('%PDF-');
  source = { name, bytes, binary };
  edited = false;
  $('editor-wrap').classList.toggle('is-binary', binary);
  editor.disabled = binary;
  if (binary) {
    editor.value = '';
    $('binary-name').textContent = name;
    $('binary-info').textContent = `${(bytes.length / 1024).toFixed(1)} KB PDF, rendered as-is (not editable)`;
  } else {
    editor.value = bytesToText(bytes);
  }
  requestRender();
}

async function loadSample(url) {
  setStatus('Loading sample…', 'busy');
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    setSource(url.split('/').pop(), new Uint8Array(await res.arrayBuffer()));
  } catch (e) {
    setStatus('Could not load sample', 'error');
    showLog(`${url}: ${e.message}`, true);
  }
}

async function loadFile(file) {
  setSource(file.name, new Uint8Array(await file.arrayBuffer()));
  const opt = document.createElement('option');
  opt.textContent = file.name;
  opt.value = '';
  $('sample').prepend(opt);
  $('sample').selectedIndex = 0;
}

// ---- Wiring ---------------------------------------------------------------

for (const [label, url] of SAMPLES) {
  const opt = document.createElement('option');
  opt.textContent = label;
  opt.value = url;
  $('sample').append(opt);
}
$('sample').addEventListener('change', (e) => e.target.value && loadSample(e.target.value));
$('open').addEventListener('click', () => $('file').click());
$('file').addEventListener('change', (e) => e.target.files[0] && loadFile(e.target.files[0]));
$('render').addEventListener('click', requestRender);
$('zoom').addEventListener('change', applyZoom);
$('show-text').addEventListener('change', (e) => $('viewer').classList.toggle('show-text', e.target.checked));
$('text-layer').addEventListener('change', requestRender);
$('reuse').addEventListener('change', requestRender);
$('transparency').addEventListener('change', requestRender);

let timer;
editor.addEventListener('input', () => {
  edited = true;
  if (!$('live').checked) return;
  clearTimeout(timer);
  timer = setTimeout(requestRender, 400);
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) {
    e.preventDefault();
    requestRender();
  }
});

const sourcePane = $('source');
sourcePane.addEventListener('dragover', (e) => {
  e.preventDefault();
  sourcePane.classList.add('drop');
});
sourcePane.addEventListener('dragleave', (e) => {
  if (!sourcePane.contains(e.relatedTarget)) sourcePane.classList.remove('drop');
});
sourcePane.addEventListener('drop', (e) => {
  e.preventDefault();
  sourcePane.classList.remove('drop');
  if (e.dataTransfer.files[0]) loadFile(e.dataTransfer.files[0]);
});

startWorker();
loadSample(SAMPLES[0][1]);
