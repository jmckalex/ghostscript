#!/usr/bin/env node
// Writes a one-page PDF exercising PDF transparency, for testing the svg
// device: constant alpha, blend modes, transparency groups, luminosity and
// alpha soft masks (including a backdrop color and a transfer function) and
// an image with an SMask.
//
//   node tools/make-transparency-pdf.mjs out.pdf

import fs from 'node:fs';

const K = 0.5523;   // Bezier circle constant
const circle = (cx, cy, r) =>
  `${cx + r} ${cy} m ${cx + r} ${cy + K * r} ${cx + K * r} ${cy + r} ${cx} ${cy + r} c ` +
  `${cx - K * r} ${cy + r} ${cx - r} ${cy + K * r} ${cx - r} ${cy} c ` +
  `${cx - r} ${cy - K * r} ${cx - K * r} ${cy - r} ${cx} ${cy - r} c ` +
  `${cx + K * r} ${cy - r} ${cx + r} ${cy - K * r} ${cx + r} ${cy} c h`;
const label = (x, y, s) => `BT /F1 9 Tf 0 g ${x} ${y} Td (${s}) Tj ET`;
const rgb = (r, g, b) => `${r} ${g} ${b} rg`;

const objs = [];   // [index] = body string or Buffer
const add = (body) => (objs.push(body), objs.length);
const stream = (dict, data) => {
  const buf = Buffer.isBuffer(data) ? data : Buffer.from(data, 'latin1');
  return Buffer.concat([Buffer.from(`<< ${dict} /Length ${buf.length} >>\nstream\n`, 'latin1'), buf,
    Buffer.from('\nendstream', 'latin1')]);
};

// Fonts
const F1 = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>');
const F2 = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>');

// A transparency group: two overlapping opaque squares, painted at 50%:
// the overlap must not be darker.
const Grp = add(stream(`/Type /XObject /Subtype /Form /BBox [0 0 200 200] ` +
  `/Group << /S /Transparency /I true /CS /DeviceRGB >>`,
  `${rgb(0.1, 0.4, 0.9)} 0 40 100 100 re f ${rgb(0.9, 0.2, 0.3)} 50 0 100 100 re f`));

// Luminosity soft mask: a black-to-white axial gradient.
const MaskGrad = add(stream(`/Type /XObject /Subtype /Form /BBox [0 0 612 792] ` +
  `/Group << /S /Transparency /CS /DeviceGray >> /Resources << /Shading << /Sh0 << /ShadingType 2 ` +
  `/ColorSpace /DeviceGray /Coords [210 0 360 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> ` +
  `/Extend [true true] >> >> >>`, '/Sh0 sh'));

// Luminosity soft mask: white text on black.
const MaskText = add(stream(`/Type /XObject /Subtype /Form /BBox [0 0 612 792] ` +
  `/Group << /S /Transparency /CS /DeviceGray >> /Resources << /Font << /F2 ${F2} 0 R >> >>`,
  `1 g BT /F2 54 Tf 400 470 Td (MASK) Tj ET`));

// Alpha soft mask: shapes at different opacities.
const MaskAlpha = add(stream(`/Type /XObject /Subtype /Form /BBox [0 0 612 792] ` +
  `/Group << /S /Transparency >> /Resources << /ExtGState << /A3 << /ca 0.3 >> /A7 << /ca 0.7 >> >> >>`,
  `0 g /A3 gs 200 300 m 340 300 l 270 420 l h f /A7 gs ${circle(300, 340, 45)} f`));

// Luminosity mask with a white backdrop (BC): everything shows except where
// the mask group paints black.
const MaskBC = add(stream(`/Type /XObject /Subtype /Form /BBox [0 0 612 792] ` +
  `/Group << /S /Transparency /CS /DeviceGray >>`, `0 g ${circle(470, 360, 30)} f`));

// An image with a soft mask (a radial alpha ramp).
const N = 48;
const pix = Buffer.alloc(N * N * 3), alpha = Buffer.alloc(N * N);
for (let y = 0; y < N; y++)
  for (let x = 0; x < N; x++) {
    const i = y * N + x;
    pix[i * 3] = (x * 255) / (N - 1); pix[i * 3 + 1] = (y * 255) / (N - 1); pix[i * 3 + 2] = 160;
    const d = Math.hypot(x - N / 2 + 0.5, y - N / 2 + 0.5) / (N / 2);
    alpha[i] = Math.max(0, Math.min(255, 255 * (1.15 - d)));
  }
const SMask = add(stream(`/Type /XObject /Subtype /Image /Width ${N} /Height ${N} /ColorSpace /DeviceGray /BitsPerComponent 8`, alpha));
const Img = add(stream(`/Type /XObject /Subtype /Image /Width ${N} /Height ${N} /ColorSpace /DeviceRGB ` +
  `/BitsPerComponent 8 /SMask ${SMask} 0 R`, pix));

const content = [
  // Striped background, so transparency is visible.
  '0.85 g', ...Array.from({ length: 40 }, (_, i) => `0 ${i * 20} 612 10 re`), 'f',
  // Row 1: constant alpha and blend modes
  label(40, 760, 'ca 0.5'),
  '/ca5 gs', rgb(1, 0, 0), circle(90, 690, 40), 'f', rgb(0, 0.8, 0), circle(130, 690, 40), 'f',
  rgb(0, 0, 1), circle(110, 650, 40), 'f', '/norm gs',
  label(220, 760, 'Multiply'),
  '/mul gs', rgb(0, 1, 1), circle(270, 690, 40), 'f', rgb(1, 0, 1), circle(310, 690, 40), 'f',
  rgb(1, 1, 0), circle(290, 650, 40), 'f', '/norm gs',
  label(400, 760, 'Screen, Difference'),
  rgb(0.2, 0.2, 0.6), '400 620 160 120 re f',
  '/scr gs', rgb(0.9, 0.3, 0), circle(450, 690, 40), 'f', '/dif gs', rgb(0.9, 0.9, 0.9), circle(510, 670, 40), 'f',
  '/norm gs',
  // Row 2: group opacity, gradient mask, text mask
  label(40, 560, 'group at 50%'),
  'q /ca5 gs 1 0 0 1 40 420 cm /Grp Do Q',
  label(220, 560, 'gradient soft mask'),
  'q /smG gs', rgb(0.9, 0.1, 0.2), '210 430 150 110 re f Q',
  label(400, 560, 'text soft mask'),
  'q /smT gs', rgb(0.1, 0.5, 0.2), '395 450 200 80 re f', rgb(0.95, 0.75, 0.1), '395 450 200 30 re f Q',
  // Row 3: image with SMask, alpha mask, backdrop mask, stroke and text alpha
  label(40, 260, 'image with SMask'),
  'q 120 0 0 120 40 120 cm /Img Do Q',
  label(220, 260, 'alpha soft mask'),
  'q /smA gs', rgb(0.5, 0.1, 0.8), '200 290 150 140 re f Q',
  label(400, 260, 'mask backdrop'),
  'q /smB gs', rgb(0.1, 0.4, 0.9), '420 310 100 100 re f Q',
  'q /str gs 8 w', rgb(0.8, 0, 0), '400 200 m 560 140 l S Q',
  'q /txt gs BT /F2 30 Tf 0 0 0.6 rg 395 150 Td (40% text) Tj ET Q',
  // Row 4: the gradient mask inverted by a transfer function
  label(220, 110, 'inverted by /TR'),
  'q /smI gs', rgb(0.9, 0.1, 0.2), '210 40 150 60 re f Q',
].join('\n');

const ExtGState =
  `/ca5 << /ca 0.5 /CA 0.5 >> /norm << /BM /Normal /ca 1 /CA 1 >> /mul << /BM /Multiply >> ` +
  `/scr << /BM /Screen >> /dif << /BM /Difference >> /str << /CA 0.5 >> /txt << /ca 0.4 >> ` +
  `/smG << /SMask << /Type /Mask /S /Luminosity /G ${MaskGrad} 0 R >> >> ` +
  `/smT << /SMask << /Type /Mask /S /Luminosity /G ${MaskText} 0 R >> >> ` +
  `/smA << /SMask << /Type /Mask /S /Alpha /G ${MaskAlpha} 0 R >> >> ` +
  `/smB << /SMask << /Type /Mask /S /Luminosity /BC [1] /G ${MaskBC} 0 R >> >> ` +
  `/smI << /SMask << /Type /Mask /S /Luminosity /G ${MaskGrad} 0 R ` +
  `/TR << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0] /N 1 >> >> >>`;
const Contents = add(stream('', content));
const Page = add(`<< /Type /Page /Parent 0 0 R /MediaBox [0 0 612 792] /Contents ${Contents} 0 R ` +
  `/Group << /S /Transparency /CS /DeviceRGB >> ` +
  `/Resources << /Font << /F1 ${F1} 0 R /F2 ${F2} 0 R >> /ExtGState << ${ExtGState} >> ` +
  `/XObject << /Grp ${Grp} 0 R /Img ${Img} 0 R >> >> >>`);
const Pages = add(`<< /Type /Pages /Kids [${Page} 0 R] /Count 1 >>`);
objs[Page - 1] = objs[Page - 1].replace('/Parent 0 0 R', `/Parent ${Pages} 0 R`);
const Catalog = add(`<< /Type /Catalog /Pages ${Pages} 0 R >>`);

const parts = [Buffer.from('%PDF-1.7\n%\xe2\xe3\xcf\xd3\n', 'latin1')];
const offsets = [];
let pos = parts[0].length;
objs.forEach((body, i) => {
  const b = Buffer.concat([Buffer.from(`${i + 1} 0 obj\n`, 'latin1'),
    Buffer.isBuffer(body) ? body : Buffer.from(body, 'latin1'), Buffer.from('\nendobj\n', 'latin1')]);
  offsets.push(pos);
  parts.push(b);
  pos += b.length;
});
const xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n` +
  offsets.map((o) => `${String(o).padStart(10, '0')} 00000 n \n`).join('') +
  `trailer\n<< /Size ${objs.length + 1} /Root ${Catalog} 0 R >>\nstartxref\n${pos}\n%%EOF\n`;
parts.push(Buffer.from(xref, 'latin1'));
fs.writeFileSync(process.argv[2] || 'transparency.pdf', Buffer.concat(parts));
