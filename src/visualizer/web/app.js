// app.js - termvideo codec visualizer UI. Fetches /trace.json once, renders the
// 80x24 cell grid on a canvas, and steps stage-by-stage then frame-by-frame
// through how the codec stored each frame. All decisions come from the trace; the
// codec itself is the source of truth (captured from the real RD tree).

// ---- Braille glyph table (mirrors src/common/glyphset.h TVID_MONO_PATTERN64) ----
// glyph index -> 8-bit Braille dot pattern; dot bit -> (row,col) per the header.
const PATTERN64 = [
  0x00,0xff,0xc0,0x09,0x1b,0x3f,0x08,0xe4,0x40,0x80,0x01,0x7f,0xbf,0xf6,0xb8,0x3b,
  0x1f,0x19,0xc4,0xf7,0xe0,0xfe,0x47,0x0b,0x18,0x03,0x44,0x5f,0x04,0xa0,0x24,0x12,
  0x10,0x02,0xe6,0xbb,0xf4,0x20,0xe7,0xb9,0xfc,0x38,0x36,0x30,0x07,0x06,0xa4,0xf0,
  0xdb,0x0f,0xf8,0x46,0x4f,0x32,0x39,0xc7,0x1a,0x13,0x3e,0x37,0xb0,0xc8,0xc6,0xc9,
];
// dot bit -> [row, col]
const DOT = [[0,0],[1,0],[2,0],[0,1],[1,1],[2,1],[3,0],[3,1]];

function lumaValue(lvl) { return Math.round(lvl * 255 / 3); }

// xterm-256 -> rgb (16 ansi + 6x6x6 cube + 24 grays), matches xterm256.h.
const ANSI16 = [
  [0,0,0],[205,0,0],[0,205,0],[205,205,0],[0,0,238],[205,0,205],[0,205,205],[229,229,229],
  [127,127,127],[255,0,0],[0,255,0],[255,255,0],[92,92,255],[255,0,255],[0,255,255],[255,255,255],
];
function xterm(idx) {
  if (idx < 16) return ANSI16[idx];
  if (idx < 232) {
    idx -= 16;
    const r = Math.floor(idx / 36), g = Math.floor((idx % 36) / 6), b = idx % 6;
    const v = c => (c ? c * 40 + 55 : 0);
    return [v(r), v(g), v(b)];
  }
  const g = (idx - 232) * 10 + 8;
  return [g, g, g];
}

const MODE_COLOR = { SKIP:'#2c3038', SOLID:'#4f8a4f', RAW:'#c25b5b', PAL2:'#b58a3c', SHIFT:'#7a5fc2' };
const STAGES = [
  'input / quantize', 'quadtree split', 'leaf modes', 'plane assembly', 'decode reassembly'
];

let T = null;          // index: metadata + per-frame light summaries (no heavy data)
let frame = 0;         // current frame index
let stage = 0;         // current stage 0..4
let selLeaf = null;    // selected leaf object
let FD = null;         // heavy data for the *current* frame (leaves, fb, colfb)
const fcache = new Map();   // frame index -> heavy data (fetched on demand)

const $ = id => document.getElementById(id);

// Fetch (and cache) one frame's heavy data. The index never carries leaves or
// framebuffers, so the browser only ever holds the frames it has visited.
async function getFrame(i) {
  if (fcache.has(i)) return fcache.get(i);
  const d = await (await fetch(`/frame/${i}.json`)).json();
  fcache.set(i, d);
  return d;
}

async function main() {
  T = await (await fetch('/trace.json')).json();
  $('meta').textContent =
    `${T.cols}x${T.rows} @ ${T.fps}fps  lambda=${T.lambda}` +
    (T.shift ? `  shift=${T.shift}` : '') +
    (T.color ? '  color' : '  mono') +
    `  ·  ${T.frames.length} frames  ·  ~${(T.total_bytes/1024).toFixed(1)} KB body`;
  $('frame-slider').max = T.frames.length - 1;
  buildLegend();
  bindControls();
  renderPlanes();
  FD = await getFrame(0);
  render();
}

function buildLegend() {
  const L = $('legend'); L.innerHTML = '';
  for (const m of ['SKIP','SHIFT','SOLID','PAL2','RAW']) {
    const c = document.createElement('span'); c.className = 'chip';
    c.innerHTML = `<span class="sw" style="background:${MODE_COLOR[m]}"></span>${m}`;
    L.appendChild(c);
  }
}

function bindControls() {
  $('frame-first').onclick = () => setFrame(0);
  $('frame-prev').onclick  = () => setFrame(frame - 1);
  $('frame-next').onclick  = () => setFrame(frame + 1);
  $('frame-last').onclick  = () => setFrame(T.frames.length - 1);
  $('frame-slider').oninput = e => setFrame(+e.target.value);
  $('stage-prev').onclick = () => setStage(stage - 1);
  $('stage-next').onclick = () => setStage(stage + 1);
  document.addEventListener('keydown', e => {
    if (e.key === 'ArrowRight') step(1);
    else if (e.key === 'ArrowLeft') step(-1);
  });
  $('grid').onclick = onCanvasClick;
}

// stage-then-frame stepping: advance stage; at the last stage, roll to next frame.
function step(dir) {
  if (dir > 0) {
    if (stage < STAGES.length - 1) setStage(stage + 1);
    else if (frame < T.frames.length - 1) gotoFrame(frame + 1, 0);
  } else {
    if (stage > 0) setStage(stage - 1);
    else if (frame > 0) gotoFrame(frame - 1, STAGES.length - 1);
  }
}
function setFrame(f) { gotoFrame(f, stage); }

// Move to a frame (fetching its heavy data first) and optionally a stage.
async function gotoFrame(f, s) {
  f = Math.max(0, Math.min(T.frames.length - 1, f));
  if (f === frame && FD) { if (s !== stage) setStage(s); return; }
  frame = f; stage = s; selLeaf = null;
  $('frame-slider').value = frame;
  FD = await getFrame(frame);
  if (frame === f) render();   // ignore a stale fetch if the user moved on
}
function setStage(s) {
  s = Math.max(0, Math.min(STAGES.length - 1, s));
  stage = s; render();
}

// ---- rendering ----
function geom() {
  const cw = $('grid').width / T.cols, ch = $('grid').height / T.rows;
  return { cw, ch };
}

function drawCell(ctx, x, y, cell, hue, cw, ch) {
  const lvl = cell >> 6, glyph = cell & 63;
  const pat = PATTERN64[glyph];
  let rgb = [lumaValue(lvl), lumaValue(lvl), lumaValue(lvl)];
  if (hue != null) {
    const [r, g, b] = xterm(hue), s = lvl / 3;
    rgb = [Math.round(r * s), Math.round(g * s), Math.round(b * s)];
  }
  ctx.fillStyle = `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`;
  const dw = cw / 2, dh = ch / 4;
  for (let bit = 0; bit < 8; bit++) {
    if (pat & (1 << bit)) {
      const [r, c] = DOT[bit];
      ctx.fillRect(x + c * dw, y + r * dh, Math.ceil(dw), Math.ceil(dh));
    }
  }
}

function render() {
  const fr = T.frames[frame];   // light summary (index)
  const fd = FD;                // heavy data for this frame (leaves, fb, colfb)
  $('frame-label').textContent = `frame ${frame}${fr.keyframe ? ' (key)' : ''}`;
  $('stage-label').textContent = `${stage + 1}/5 ${STAGES[stage]}`;
  const ctx = $('grid').getContext('2d');
  const { cw, ch } = geom();
  ctx.fillStyle = '#0c0d10';
  ctx.fillRect(0, 0, $('grid').width, $('grid').height);

  // The framebuffer the decoder holds for this frame (from the fetched chunk).
  for (let yy = 0; yy < T.rows; yy++)
    for (let xx = 0; xx < T.cols; xx++) {
      const i = yy * T.cols + xx;
      const hue = (T.color && fd.colfb) ? fd.colfb[i] : null;
      drawCell(ctx, xx * cw, yy * ch, fd.fb[i], hue, cw, ch);
    }

  // Stage overlays.
  if (!fr.keyframe) {
    if (stage === 1 || stage === 2) {
      for (const lf of fd.leaves) {
        const x = lf.x * cw, y = lf.y * ch, w = lf.s * cw, h = lf.s * ch;
        if (stage === 2) {
          const m = lf.mode === 'SKIP' && lf.mvx !== undefined ? 'SHIFT' : lf.mode;
          ctx.fillStyle = MODE_COLOR[m] + '88';
          ctx.fillRect(x, y, w, h);
        }
        ctx.strokeStyle = '#000a'; ctx.lineWidth = 1;
        ctx.strokeRect(x + .5, y + .5, w - 1, h - 1);
      }
    }
    if (selLeaf) {
      ctx.strokeStyle = '#5aa9e6'; ctx.lineWidth = 2;
      ctx.strokeRect(selLeaf.x * cw + 1, selLeaf.y * ch + 1,
                     selLeaf.s * cw - 2, selLeaf.s * ch - 2);
    }
  }
  renderFrameInfo();
  renderStageInfo();
  renderLeaf();
}

function renderFrameInfo() {
  const fr = T.frames[frame];
  const total = fr.bytes.structure + fr.bytes.cell + fr.bytes.color;
  const leaves = fr.n_skip + fr.n_solid + fr.n_raw + fr.n_pal2;
  $('frame-info').innerHTML =
    kv('type', fr.keyframe ? 'keyframe' : 'delta') +
    kv('leaves', leaves) +
    kv('SKIP', fr.n_skip) + kv('SOLID', fr.n_solid) +
    kv('RAW', fr.n_raw) + kv('PAL2', fr.n_pal2) +
    `<div style="margin-top:8px"></div>` +
    kv('struct B', fr.bytes.structure) +
    kv('cell B', fr.bytes.cell) +
    (T.color ? kv('color B', fr.bytes.color) : '') +
    kv('total B', total);
}

function renderStageInfo() {
  const fr = T.frames[frame];
  let html = '';
  switch (stage) {
    case 0:
      html = 'Per-cell 2x4 luma quantized to (luma,glyph). The grid shows the ' +
             'decoded result; on the keyframe this is the absolute cell grid.';
      break;
    case 1:
      html = fr.keyframe ? 'Keyframe: stored as raw cells, no quadtree.' :
             `Quadtree of ${FD.leaves.length} leaves. Each 8x8 superblock splits ` +
             `down to 1x1; one bit per split. Boundaries drawn in black.`;
      break;
    case 2:
      html = fr.keyframe ? 'Keyframe: no per-leaf modes.' :
             'Each leaf coded by its cheapest mode (cost = lambda*bits + ' +
             'distortion). Regions tinted by mode; click one for detail.';
      break;
    case 3:
      html = 'Bytes this frame routed into the split planes:' +
             plane('structure', fr.bytes.structure) +
             plane('cell', fr.bytes.cell) +
             (T.color ? plane('color', fr.bytes.color) : '');
      break;
    case 4:
      html = 'Decoder replays the same tree: per-plane entropy decode, then the ' +
             'cheap fill/copy per leaf. This is byte-identical to what the player ' +
             'renders (verifiable via TVID_DUMP=1).';
      break;
  }
  $('stage-info').innerHTML = html;
}

function plane(name, b) {
  const fr = T.frames[frame];
  const tot = fr.bytes.structure + fr.bytes.cell + fr.bytes.color || 1;
  return `<div class="kv"><span>${name}</span><span>${b} B</span></div>` +
         `<div class="bar"><i style="width:${100*b/tot}%"></i></div>`;
}

function renderLeaf() {
  if (!selLeaf) { $('leaf-info').textContent = 'click a region'; return; }
  const lf = selLeaf;
  const isShift = lf.mode === 'SKIP' && lf.mvx !== undefined;
  let html = kv('mode', isShift ? 'SHIFT' : lf.mode) +
             kv('pos', `${lf.x},${lf.y}`) + kv('size', `${lf.s}x${lf.s}`) +
             kv('rd cost', lf.rd);
  if (isShift) html += kv('motion', `${lf.mvx},${lf.mvy}`);
  if (lf.mode === 'SOLID') html += kv('cell', lf.cell) + kv('luma', lf.luma) + kv('glyph', lf.glyph);
  if (lf.mode === 'PAL2') html += kv('pal0', lf.pal0) + kv('pal1', lf.pal1);
  html += renderLeafBytes(lf);
  $('leaf-info').innerHTML = html;
}

// The exact wire representation this region contributes, per plane. Highlights
// the plane the current stage is about (structure on the split/mode stages, cell
// on quantize/assembly, color when present), but always shows all three so the
// full byte footprint of a selected region is visible at any stage.
function renderLeafBytes(lf) {
  const sb = lf.sbits || '';
  const cb = lf.cbytes || [];
  const clb = lf.clbytes || [];
  // which plane this stage emphasizes
  const hot = stage === 1 || stage === 2 ? 'structure'
            : stage === 3 ? 'all'
            : stage === 0 || stage === 4 ? 'cell' : '';
  const sec = (name, on, body) =>
    `<div class="bytesec${on ? ' hot' : ''}"><div class="byteshd">${name}</div>${body}</div>`;
  const structBits = sb.length
    ? `<div class="bits" title="MSB-first from bit ${lf.sbit0}">${groupBits(sb)}</div>` +
      `<div class="byteslbl">${sb.length} bit${sb.length===1?'':'s'} @ off ${lf.sbit0}</div>`
    : `<div class="byteslbl">no structure bits</div>`;
  const cellBytes = cb.length
    ? `<div class="bytes">${hexList(cb)}</div>` +
      `<div class="byteslbl">${cb.length} byte${cb.length===1?'':'s'}` +
      (lf.mode === 'RAW' ? ` (${lf.s}x${lf.s} raster)`
       : lf.mode === 'SOLID' ? ' (palette)'
       : lf.mode === 'PAL2' ? ' (2 palette cells)' : '') + `</div>`
    : `<div class="byteslbl">no cell bytes (${lf.mode} stores nothing here)</div>`;
  const colorBytes = clb.length
    ? `<div class="bytes">${hexList(clb)}</div>` +
      `<div class="byteslbl">${clb.length} hue byte${clb.length===1?'':'s'}</div>`
    : '';
  let out = `<div class="bytestitle">wire bytes for this region</div>`;
  out += sec('structure plane', hot==='structure'||hot==='all', structBits);
  out += sec('cell plane', hot==='cell'||hot==='all', cellBytes);
  if (T.color) out += sec('color plane', hot==='all', colorBytes || '<div class="byteslbl">no hue bytes</div>');
  return out;
}

function groupBits(s) {
  // space every 4 bits for readability
  return s.replace(/(.{4})/g, '$1 ').trim();
}
function hexList(arr) {
  return arr.map(b => b.toString(16).padStart(2, '0')).join(' ');
}

function renderPlanes() {
  let html = '';
  const M = ['raw','lzss','huffman','range'];
  for (const p of T.planes) {
    if (p.method < 0) continue;
    const pct = p.raw ? (100 * p.coded / p.raw).toFixed(0) : 0;
    html += `<div class="kv"><span>${p.name}</span>` +
            `<span>${(p.coded/1024).toFixed(1)}K ${M[p.method]||'?'}</span></div>` +
            `<div class="bar"><i style="width:${pct}%"></i></div>` +
            `<div class="kv"><span></span><span style="color:var(--muted)">` +
            `${p.raw} B raw &rarr; ${pct}%</span></div>`;
  }
  $('plane-info').innerHTML = html;
}

function onCanvasClick(e) {
  if (T.frames[frame].keyframe || !FD) return;
  const rect = $('grid').getBoundingClientRect();
  const { cw, ch } = geom();
  const cx = Math.floor((e.clientX - rect.left) * ($('grid').width / rect.width) / cw);
  const cy = Math.floor((e.clientY - rect.top) * ($('grid').height / rect.height) / ch);
  selLeaf = FD.leaves.find(l =>
    cx >= l.x && cx < l.x + l.s && cy >= l.y && cy < l.y + l.s) || null;
  if (stage < 2) setStage(2); else render();
}

function kv(k, v) {
  return `<div class="kv"><span>${k}</span><span>${v}</span></div>`;
}

main();
