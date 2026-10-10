#!/usr/bin/env node
// Writes a two-page PDF exercising the svg device's handling of tiling
// patterns, fill-and-stroke under transparency, JPEG images (passed through,
// and one drawn several times), links, optional content (layers) and the
// document information dictionary.
//
//   node tools/make-features-pdf.mjs out.pdf
//
// The JPEGs are test/fixtures/photo.jpg and photo-gray.jpg, made by
// Ghostscript's jpeg and jpeggray devices from test/fixtures/photo.ps.

import fs from 'node:fs';

const fixture = (f) => fs.readFileSync(new URL(`../test/fixtures/${f}`, import.meta.url));
const K = 0.5523;   // Bezier circle constant
const circle = (cx, cy, r) =>
  `${cx + r} ${cy} m ${cx + r} ${cy + K * r} ${cx + K * r} ${cy + r} ${cx} ${cy + r} c ` +
  `${cx - K * r} ${cy + r} ${cx - r} ${cy + K * r} ${cx - r} ${cy} c ` +
  `${cx - r} ${cy - K * r} ${cx - K * r} ${cy - r} ${cx} ${cy - r} c ` +
  `${cx + K * r} ${cy - r} ${cx + r} ${cy - K * r} ${cx + r} ${cy} c h`;
const label = (x, y, s) => `BT /F1 9 Tf 0 g ${x} ${y} Td (${s}) Tj ET`;
const rgb = (r, g, b) => `${r} ${g} ${b} rg`;
const RGB = (r, g, b) => `${r} ${g} ${b} RG`;
const stripes = (x, y, w, h) => Array.from({ length: Math.ceil(h / 12) },
  (_, i) => `${x} ${y + i * 12} ${w} 6 re`).join(' ') + ' f';

const objs = [];   // [index] = body string or Buffer
const add = (body) => (objs.push(body), objs.length);
const reserve = () => add('');
const stream = (dict, data) => {
  const buf = Buffer.isBuffer(data) ? data : Buffer.from(data, 'latin1');
  return Buffer.concat([Buffer.from(`<< ${dict} /Length ${buf.length} >>\nstream\n`, 'latin1'), buf,
    Buffer.from('\nendstream', 'latin1')]);
};

// The size and components of a baseline JPEG, from its SOF0 marker.
function jpegInfo(b) {
  for (let i = 2; i < b.length;) {
    const marker = b[i + 1], len = b.readUInt16BE(i + 2);
    if (marker >= 0xc0 && marker <= 0xc2)
      return { h: b.readUInt16BE(i + 5), w: b.readUInt16BE(i + 7), n: b[i + 9] };
    i += 2 + len;
  }
  throw new Error('no SOF marker');
}
const jpegXObject = (data) => {
  const { w, h, n } = jpegInfo(data);
  return add(stream(`/Type /XObject /Subtype /Image /Width ${w} /Height ${h} ` +
    `/ColorSpace /${n === 1 ? 'DeviceGray' : 'DeviceRGB'} /BitsPerComponent 8 /Filter /DCTDecode`, data));
};

const F1 = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>');
const F2 = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>');

// Tiling patterns. P1: coloured (PaintType 1), a 20pt cell with a circle and
// a square. P2: uncoloured (PaintType 2) diagonal hatching, painted in the
// colour given with scn. P3: P1's cell, rotated 30 degrees and scaled 0.7.
const cell = `${rgb(1, 0.95, 0.75)} 0 0 20 20 re f ${rgb(0.85, 0.15, 0.2)} ${circle(10, 10, 6)} f ` +
  `${rgb(0.1, 0.3, 0.8)} 15 15 4 4 re f`;
const P1 = add(stream('/PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 20 20] /XStep 20 /YStep 20 ' +
  '/Resources << >>', cell));
const P2 = add(stream('/PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 10 10] /XStep 10 /YStep 10 ' +
  '/Resources << >>', '2 w 0 J -1 -1 m 11 11 l S -1 9 m 1 11 l S 9 -1 m 11 1 l S'));
const P3 = add(stream('/PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 20 20] /XStep 20 /YStep 20 ' +
  '/Matrix [0.6062 0.35 -0.35 0.6062 0 0] /Resources << >>', cell));

// JPEG images: Jpg is drawn three times (twice at the same size).
const Jpg = jpegXObject(fixture('photo.jpg'));
const JpgG = jpegXObject(fixture('photo-gray.jpg'));

// Optional content: two visible layers and one that is off by default.
const OCGrid = add('<< /Type /OCG /Name (Grid) >>');
const OCNotes = add('<< /Type /OCG /Name (Notes) >>');
const OCHidden = add('<< /Type /OCG /Name (Hidden) >>');

const Page1 = reserve();
const Page2 = reserve();

// A sticky note with an appearance: links aside, annotations are drawn.
const NoteAP = add(stream('/Type /XObject /Subtype /Form /BBox [0 0 20 20]',
  `${rgb(1, 0.85, 0.2)} 0 0 20 20 re f ${RGB(0.5, 0.4, 0)} 1 w 0.5 0.5 19 19 re S ` +
  `0 G 4 14 m 16 14 l 4 10 m 16 10 l 4 6 m 12 6 l S`));
const Note = add(`<< /Type /Annot /Subtype /Text /Rect [560 140 580 160] /Contents (A note) ` +
  `/AP << /N ${NoteAP} 0 R >> /F 4 >>`);
const LinkURI = add('<< /Type /Annot /Subtype /Link /Rect [38 150 170 166] /Border [0 0 0] ' +
  '/A << /S /URI /URI (https://ghostscript.com/) >> >>');
const LinkPage2 = add(`<< /Type /Annot /Subtype /Link /Rect [38 122 140 138] /Border [0 0 0] ` +
  `/Dest [${Page2} 0 R /XYZ 0 792 0] >>`);
const LinkBack = add(`<< /Type /Annot /Subtype /Link /Rect [38 680 160 696] /Border [0 0 0] ` +
  `/Dest [${Page1} 0 R /XYZ 0 792 0] >>`);

const content1 = [
  'BT /F2 18 Tf 0 g 40 750 Td (SVG device features) Tj ET',
  // Row 1: tiling patterns
  label(40, 715, 'coloured tiling pattern'),
  '/Pattern cs /P1 scn 40 580 150 120 re f',
  label(210, 715, 'uncoloured, in two colours'),
  '/CSP cs 0.8 0.1 0.1 /P2 scn 210 580 70 120 re f 0.1 0.2 0.8 /P2 scn 290 580 70 120 re f',
  label(380, 715, 'rotated; text; stroke'),
  '/Pattern cs /P3 scn 380 650 90 50 re f',
  '/Pattern cs /P1 scn BT /F2 44 Tf 380 596 Td (Tiles) Tj ET',
  '/Pattern CS /P1 SCN 12 w 490 690 m 570 600 l S',
  // Row 2: fill and stroke under transparency (B), over stripes
  `${rgb(0.8, 0.8, 0.8)} ${stripes(40, 400, 530, 140)}`,
  label(40, 545, 'fill + stroke, ca = CA = 0.5'),
  label(220, 545, 'ca 0.3, CA 0.9'),
  label(400, 545, 'opaque'),
  `q /h gs ${rgb(0.1, 0.5, 0.9)} ${RGB(0.9, 0.2, 0.1)} 14 w ${circle(110, 470, 50)} B Q`,
  `q /m gs ${rgb(0.1, 0.5, 0.9)} ${RGB(0.9, 0.2, 0.1)} 14 w ${circle(290, 470, 50)} B Q`,
  `q ${rgb(0.1, 0.5, 0.9)} ${RGB(0.9, 0.2, 0.1)} 14 w ${circle(470, 470, 50)} B Q`,
  // Row 3: JPEG images
  label(40, 370, 'one JPEG, three placements'),
  'q 96 0 0 64 40 290 cm /Jpg Do Q q 96 0 0 64 150 290 cm /Jpg Do Q q 144 0 0 96 260 258 cm /Jpg Do Q',
  label(420, 370, 'greyscale JPEG'),
  'q 144 0 0 96 420 258 cm /JpgG Do Q',
  // Row 4: links, layers and an annotation
  label(40, 210, 'links'),
  `BT /F1 12 Tf ${rgb(0.1, 0.3, 0.8)} 40 154 Td (ghostscript.com) Tj 0 -28 Td (Go to page 2) Tj ET`,
  label(220, 210, 'layers: Grid, Notes (Hidden is off)'),
  `/OC /Grid BDC ${RGB(0.6, 0.7, 0.9)} 0.5 w ` +
    Array.from({ length: 7 }, (_, i) => `${220 + i * 20} 60 m ${220 + i * 20} 180 l`).join(' ') + ' ' +
    Array.from({ length: 7 }, (_, i) => `220 ${60 + i * 20} m 340 ${60 + i * 20} l`).join(' ') + ' S EMC',
  `/OC /Notes BDC BT /F2 14 Tf ${rgb(0.2, 0.5, 0.2)} 230 115 Td (Notes layer) Tj ET EMC`,
  `/OC /Hidden BDC ${RGB(1, 0, 0)} 6 w 360 60 m 420 180 l 420 60 m 360 180 l S EMC`,
  label(500, 210, 'a note'),
].join('\n');

const content2 = [
  'BT /F2 18 Tf 0 g 40 750 Td (Page 2) Tj ET',
  `BT /F1 12 Tf ${rgb(0.1, 0.3, 0.8)} 40 684 Td (Back to page 1) Tj ET`,
].join('\n');

const resources = `/Resources << /Font << /F1 ${F1} 0 R /F2 ${F2} 0 R >> ` +
  `/Pattern << /P1 ${P1} 0 R /P2 ${P2} 0 R /P3 ${P3} 0 R >> ` +
  `/ColorSpace << /CSP [/Pattern /DeviceRGB] >> ` +
  `/ExtGState << /h << /ca 0.5 /CA 0.5 >> /m << /ca 0.3 /CA 0.9 >> >> ` +
  `/XObject << /Jpg ${Jpg} 0 R /JpgG ${JpgG} 0 R >> ` +
  `/Properties << /Grid ${OCGrid} 0 R /Notes ${OCNotes} 0 R /Hidden ${OCHidden} 0 R >> >>`;
const Contents1 = add(stream('', content1));
const Contents2 = add(stream('', content2));
const Pages = add('');
objs[Page1 - 1] = `<< /Type /Page /Parent ${Pages} 0 R /MediaBox [0 0 612 792] /Contents ${Contents1} 0 R ` +
  `${resources} /Annots [${LinkURI} 0 R ${LinkPage2} 0 R ${Note} 0 R] >>`;
objs[Page2 - 1] = `<< /Type /Page /Parent ${Pages} 0 R /MediaBox [0 0 612 792] /Contents ${Contents2} 0 R ` +
  `${resources} /Annots [${LinkBack} 0 R] >>`;
objs[Pages - 1] = `<< /Type /Pages /Kids [${Page1} 0 R ${Page2} 0 R] /Count 2 >>`;
const Catalog = add(`<< /Type /Catalog /Pages ${Pages} 0 R /OCProperties << ` +
  `/OCGs [${OCGrid} 0 R ${OCNotes} 0 R ${OCHidden} 0 R] ` +
  `/D << /Order [${OCGrid} 0 R ${OCNotes} 0 R ${OCHidden} 0 R] /OFF [${OCHidden} 0 R] >> >> >>`);
const Info = add('<< /Title (SVG device features) /Author (Ghostscript WASM tests) ' +
  '/Subject (Patterns, JPEG, links, layers and metadata) /Keywords (svg, ghostscript, test) >>');

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
  `trailer\n<< /Size ${objs.length + 1} /Root ${Catalog} 0 R /Info ${Info} 0 R >>\nstartxref\n${pos}\n%%EOF\n`;
parts.push(Buffer.from(xref, 'latin1'));
fs.writeFileSync(process.argv[2] || 'features.pdf', Buffer.concat(parts));
