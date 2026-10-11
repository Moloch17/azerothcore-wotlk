// Forge route viewer: the page. The server (server.py) serves the runs, each evaluation's episodes and routes, and a
// background per map drawn from its baked scene; everything here is drawing and filtering on a canvas.
'use strict';

const $ = (id) => document.getElementById(id);
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

const state = {
  runs: [], run: null, steps: null, policy: null, map: null,
  episodes: [], blocks: [], stepSeconds: 0.25,
  bg: null, bgImg: null,
  view: { scale: 1, x: 0, y: 0 },          // screen = world_px * scale + offset
  outcome: 'all', colourBy: 'outcome', classesOff: new Set(), seed: '',
  selected: null, play: { i: 0, playing: false, last: 0 },
};

const CLASS_COLOURS = {
  warrior: '#c79c6e', paladin: '#f58cba', hunter: '#abd473', rogue: '#fff569', priest: '#e8e8e8',
  deathknight: '#c41f3b', shaman: '#2f8fff', mage: '#69ccf0', warlock: '#9482c9', druid: '#ff7d0a',
};

async function api(path) {
  const r = await fetch(path);
  const data = await r.json();
  if (!r.ok) throw new Error(data.error || r.statusText);
  return data;
}
function setStatus(text) { $('status').textContent = text || ''; }

// ---- selection: run, evaluation, policy, map ------------------------------------------------------------------------

function fillSelect(el, items, value) {
  el.innerHTML = '';
  for (const [v, label] of items) {
    const o = document.createElement('option');
    o.value = v; o.textContent = label;
    el.appendChild(o);
  }
  if (value != null && items.some(([v]) => String(v) === String(value))) el.value = value;
}
function fmtSteps(s) { return s >= 1e6 ? (s / 1e6).toFixed(s % 1e6 ? 1 : 0) + 'M' : s >= 1e3 ? Math.round(s / 1e3) + 'k' : String(s); }

function readHash() {
  const p = new URLSearchParams(location.hash.slice(1));
  return { run: p.get('run'), steps: p.get('steps'), policy: p.get('policy'), map: p.get('map'), seed: p.get('seed') };
}
function writeHash() {
  const p = new URLSearchParams();
  if (state.run) p.set('run', state.run);
  if (state.steps != null) p.set('steps', state.steps);
  if (state.policy) p.set('policy', state.policy);
  if (state.map != null) p.set('map', state.map);
  if (state.selected) p.set('seed', `${state.selected.seed}.${state.selected.agent}`);
  history.replaceState(null, '', '#' + p.toString());
}

async function loadRuns() {
  state.runs = await api('/api/runs');
  if (!state.runs.length) { setStatus('No run with evaluation traces in the runs folder.'); return; }
  const h = readHash();
  fillSelect($('run'), state.runs.map((r) => [r.name, r.name]), h.run);
  state.run = $('run').value;
  fillEvals(h.steps, h.policy);
  await loadEvaluation(h.map, h.seed);
}

function currentRun() { return state.runs.find((r) => r.name === state.run); }
function fillEvals(stepsWanted, policyWanted) {
  const run = currentRun();
  const evals = run.evals.slice().reverse();
  fillSelect($('eval'), evals.map((e) => [e.steps, fmtSteps(e.steps) + ' steps']), stepsWanted ?? evals[0].steps);
  state.steps = Number($('eval').value);
  const ev = run.evals.find((e) => e.steps === state.steps);
  fillSelect($('policy'), ev.policies.map((p) => [p, p]), policyWanted ?? ev.policies[0]);
  state.policy = $('policy').value;
}

async function loadEvaluation(mapWanted, seedWanted) {
  setStatus('Loading routes…');
  stopPlay();
  state.selected = null;
  const q = `run=${encodeURIComponent(state.run)}&steps=${state.steps}&policy=${encodeURIComponent(state.policy)}`;
  const [eps, tr, media] = await Promise.all([api('/api/episodes?' + q), api('/api/trace?' + q),
    api('/api/media?' + q).catch(() => [])]);
  state.episodes = eps;
  state.stepSeconds = tr.step_seconds || 0.25;
  state.blocks = tr.blocks.filter((b) => b.episode && b.x && b.x.length);
  // The evaluation's videos are of these very episodes (the learner's evaluation, filmed by the sim): by seed and agent.
  const videos = new Map(media.map((v) => [`${v.seed}.${v.agent}`, v]));
  for (const b of state.blocks) b.video = videos.get(`${b.seed}.${b.agent}`) || null;
  state.videoCount = media.length;
  state.hover = null;
  const maps = [...new Set(state.blocks.map((b) => b.episode.map))].sort((a, b) => a - b);
  const tabs = $('maps');
  tabs.innerHTML = '';
  for (const m of maps) {
    const b = document.createElement('button');
    b.textContent = mapName(m);
    b.dataset.map = m;
    b.onclick = () => selectMap(m);
    tabs.appendChild(b);
  }
  const target = maps.includes(Number(mapWanted)) ? Number(mapWanted) : maps[0];
  if (target == null) { setStatus('This evaluation recorded no routes.'); return; }
  await selectMap(target, true);
  if (seedWanted) {
    const [s, a] = seedWanted.split('.').map(Number);
    const blk = state.blocks.find((b) => b.seed === s && b.agent === (a || 0) && b.episode.map === state.map);
    if (blk) selectEpisode(blk);
  }
}

const MAP_NAMES = { 34: 'The Stockade', 36: 'The Deadmines', 389: 'Ragefire Chasm', 43: 'Wailing Caverns' };
function mapName(m) { return MAP_NAMES[m] || `Map ${m}`; }

async function selectMap(m, keepView, band) {
  state.map = m;
  for (const b of $('maps').children) b.classList.toggle('on', Number(b.dataset.map) === m);
  setStatus('Drawing the map…');
  let q = `run=${encodeURIComponent(state.run)}&steps=${state.steps}&policy=${encodeURIComponent(state.policy)}&map=${m}`;
  if (band) q += `&zlo=${band[0]}&zhi=${band[1]}`;
  state.bg = await api('/api/background?' + q);
  $('zlo').value = state.bg.zlo; $('zhi').value = state.bg.zhi;
  $('zlo').title = $('zhi').title = `routes span ${state.bg.zmin} to ${state.bg.zmax}`;
  state.bgImg = await new Promise((res, rej) => {
    const im = new Image(); im.onload = () => res(im); im.onerror = rej; im.src = state.bg.image;
  });
  state.heat = null;
  buildClassChips();
  renderSummary();
  renderList();
  if (!keepView || !state.viewSet) fit();
  setStatus('');
  writeHash();
  draw();
}

// ---- coordinates ----------------------------------------------------------------------------------------------------

function worldToPx(x, y) {               // background pixel space (north up)
  return [(x - state.bg.x0) / state.bg.px, (state.bg.y1 - y) / state.bg.px];
}
function pxToScreen(px, py) { const v = state.view; return [px * v.scale + v.x, py * v.scale + v.y]; }
function screenToPx(sx, sy) { const v = state.view; return [(sx - v.x) / v.scale, (sy - v.y) / v.scale]; }

function fit() {
  const c = $('canvas'), im = state.bgImg;
  if (!im) return;
  const w = c.clientWidth, h = c.clientHeight;
  const s = Math.min(w / im.width, h / im.height) * 0.94;
  state.view = { scale: s, x: (w - im.width * s) / 2, y: (h - im.height * s) / 2 };
  state.viewSet = true;
  draw();
}

// ---- filters --------------------------------------------------------------------------------------------------------

function visibleBlocks() {
  const seed = state.seed.trim();
  return state.blocks.filter((b) => {
    const e = b.episode;
    if (e.map !== state.map) return false;
    if (state.outcome === 'found' && !e.found) return false;
    if (state.outcome === 'missed' && e.found) return false;
    if (state.classesOff.has(e.class)) return false;
    if (seed && String(b.seed) !== seed) return false;
    return true;
  });
}

function buildClassChips() {
  const box = $('classes');
  box.innerHTML = '';
  const classes = [...new Set(state.blocks.filter((b) => b.episode.map === state.map).map((b) => b.episode.class))].sort();
  for (const k of classes) {
    const chip = document.createElement('button');
    chip.className = 'chip' + (state.classesOff.has(k) ? ' off' : '');
    chip.innerHTML = `<i style="background:${CLASS_COLOURS[k] || '#999'}"></i>${k}`;
    chip.onclick = () => {
      if (state.classesOff.has(k)) state.classesOff.delete(k); else state.classesOff.add(k);
      chip.classList.toggle('off');
      state.heat = null; renderSummary(); renderList(); draw();
    };
    box.appendChild(chip);
  }
}

function routeColour(b, i) {
  if (state.colourBy === 'class') return CLASS_COLOURS[b.episode.class] || '#bbb';
  if (state.colourBy === 'episode') return `hsl(${(i * 137.5) % 360} 75% 60%)`;
  return b.episode.found ? css('--found') : css('--missed');
}

// ---- side panels ----------------------------------------------------------------------------------------------------

function renderSummary() {
  const bs = visibleBlocks();
  const found = bs.filter((b) => b.episode.found);
  const times = found.map((b) => b.episode.find_seconds).filter((t) => t != null);
  const mean = times.length ? times.reduce((a, b) => a + b, 0) / times.length : null;
  const byClass = {};
  for (const b of bs) {
    const k = b.episode.class;
    byClass[k] = byClass[k] || [0, 0];
    byClass[k][1]++;
    if (b.episode.found) byClass[k][0]++;
  }
  const rows = Object.entries(byClass).sort().map(([k, [f, n]]) =>
    `<span><i class="cdot" style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${CLASS_COLOURS[k] || '#999'}"></i> ${k}</span><b>${f}/${n}</b>`).join('');
  $('summary').innerHTML = `
    <h2>${mapName(state.map)} <span class="muted">· ${fmtSteps(state.steps)} · ${state.policy}</span></h2>
    <div class="big">${bs.length ? Math.round(100 * found.length / bs.length) : 0}% found</div>
    <div class="stats">
      <span>Episodes</span><b>${found.length} / ${bs.length}</b>
      <span>Mean time to find</span><b>${mean != null ? mean.toFixed(0) + ' s' : '–'}</b>
      ${rows}
    </div>`;
}

function renderList() {
  const box = $('episodes');
  box.innerHTML = '';
  const bs = visibleBlocks().slice().sort((a, b) => (a.episode.found - b.episode.found) || (a.seed - b.seed));
  $('epCount').textContent = `(${bs.length})`;
  let selRow = null;
  for (const b of bs) {
    const e = b.episode;
    const row = document.createElement('div');
    row.className = 'ep' + (state.selected === b ? ' sel' : '');
    row.setAttribute('role', 'option');
    row.innerHTML = `<span class="dot" style="background:${e.found ? css('--found') : css('--missed')}"></span>
      <span class="seed">#${b.seed}</span>
      <span>${e.class}${e.room ? ' · ' + e.room : ''}</span>
      <span class="t">${b.video ? '<b class="vid" title="filmed: video in the media panel">▶</b> ' : ''}${e.found ? Math.round(e.find_seconds || 0) + ' s' : 'miss'}</span>`;
    row.onclick = () => selectEpisode(b);
    row.onmouseenter = () => { state.hover = b; draw(); };
    row.onmouseleave = () => { if (state.hover === b) { state.hover = null; draw(); } };
    box.appendChild(row);
    if (state.selected === b) selRow = row;
  }
  if (selRow) selRow.scrollIntoView({ block: 'nearest' });
}

// ---- drawing --------------------------------------------------------------------------------------------------------

function resizeCanvas() {
  const c = $('canvas'), dpr = window.devicePixelRatio || 1;
  c.width = Math.round(c.clientWidth * dpr);
  c.height = Math.round(c.clientHeight * dpr);
}

function heatLayer() {
  if (state.heat) return state.heat;
  const im = state.bgImg, cell = 2;                     // background pixels per heat cell (1 yd)
  const w = Math.ceil(im.width / cell), h = Math.ceil(im.height / cell);
  const grid = new Float32Array(w * h);
  for (const b of visibleBlocks()) {
    for (let i = 0; i < b.x.length; i++) {
      const [px, py] = worldToPx(b.x[i], b.y[i]);
      const gx = Math.floor(px / cell), gy = Math.floor(py / cell);
      if (gx >= 0 && gy >= 0 && gx < w && gy < h) grid[gy * w + gx] += 1;
    }
  }
  let max = 0;
  for (const v of grid) if (v > max) max = v;
  const oc = document.createElement('canvas');
  oc.width = w; oc.height = h;
  const ctx = oc.getContext('2d'), img = ctx.createImageData(w, h);
  for (let i = 0; i < grid.length; i++) {
    if (!grid[i]) continue;
    const t = Math.log1p(grid[i]) / Math.log1p(max);
    img.data.set([...inferno(t), Math.round(120 + 135 * t)], i * 4);
  }
  ctx.putImageData(img, 0, 0);
  state.heat = { canvas: oc, cell };
  return state.heat;
}

const INFERNO = [[20, 11, 52], [88, 15, 109], [147, 38, 103], [204, 66, 72], [243, 120, 25], [252, 196, 54], [252, 255, 164]];
function inferno(t) {
  const x = Math.max(0, Math.min(1, t)) * (INFERNO.length - 1), i = Math.min(INFERNO.length - 2, Math.floor(x)), f = x - i;
  return INFERNO[i].map((c, k) => Math.round(c + (INFERNO[i + 1][k] - c) * f));
}

function draw() {
  const c = $('canvas');
  if (!c.width) resizeCanvas();
  const ctx = c.getContext('2d'), dpr = window.devicePixelRatio || 1;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = css('--void');
  ctx.fillRect(0, 0, c.width, c.height);
  if (!state.bgImg) return;
  const v = state.view;
  ctx.setTransform(dpr * v.scale, 0, 0, dpr * v.scale, dpr * v.x, dpr * v.y);
  ctx.imageSmoothingEnabled = v.scale < 2;
  ctx.drawImage(state.bgImg, 0, 0);

  const bs = visibleBlocks();
  const lw = 1.6 / v.scale;
  const heat = $('lHeat').checked;
  if ($('lRoutes').checked) {
    const alpha = Number($('alpha').value) / 100 * (heat ? 0.3 : 1);
    const dim = state.selected ? 0.25 : 1;
    ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    bs.forEach((b, i) => {
      if (b === state.selected) return;
      ctx.globalAlpha = alpha * dim;
      ctx.strokeStyle = routeColour(b, i);
      ctx.lineWidth = lw;
      ctx.beginPath();
      for (let k = 0; k < b.x.length; k++) {
        const [px, py] = worldToPx(b.x[k], b.y[k]);
        if (k) ctx.lineTo(px, py); else ctx.moveTo(px, py);
      }
      ctx.stroke();
    });
    ctx.globalAlpha = 1;
  }
  if (heat) {
    const hl = heatLayer();
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(hl.canvas, 0, 0, hl.canvas.width * hl.cell, hl.canvas.height * hl.cell);
  }
  if ($('lPins').checked) {
    ctx.fillStyle = '#ff3b6b';
    const r = 2.2 / v.scale;
    for (const b of bs) {
      if (!b.wall) continue;
      for (let k = 0; k < b.x.length; k++) {
        if (!b.wall[k]) continue;
        const [px, py] = worldToPx(b.x[k], b.y[k]);
        ctx.globalAlpha = b.moving && !b.moving[k] ? 0.9 : 0.35;
        ctx.fillRect(px - r, py - r, 2 * r, 2 * r);
      }
    }
    ctx.globalAlpha = 1;
  }
  const marks = (list, fn) => list.forEach(fn);
  if ($('lStarts').checked) {
    ctx.fillStyle = css('--start');
    const r = 3.2 / v.scale;
    marks(bs, (b) => { const [px, py] = worldToPx(b.x[0], b.y[0]); ctx.beginPath(); ctx.arc(px, py, r, 0, 7); ctx.fill(); });
  }
  if ($('lEnds').checked) {
    ctx.strokeStyle = '#f2f2f2'; ctx.lineWidth = 1.5 / v.scale;
    const r = 3 / v.scale;
    marks(bs, (b) => { const n = b.x.length - 1; const [px, py] = worldToPx(b.x[n], b.y[n]);
      ctx.beginPath(); ctx.moveTo(px - r, py - r); ctx.lineTo(px + r, py + r); ctx.moveTo(px + r, py - r); ctx.lineTo(px - r, py + r); ctx.stroke(); });
  }
  if ($('lObjects').checked) {
    const r = 3.6 / v.scale;
    ctx.lineWidth = 2 / v.scale;
    marks(bs, (b) => {
      const o = b.episode.object_xyz;
      if (!o || o[0] == null) return;
      const [px, py] = worldToPx(o[0], o[1]);
      ctx.strokeStyle = b.episode.found ? css('--obj-found') : css('--obj-missed');
      ctx.strokeRect(px - r, py - r, 2 * r, 2 * r);
    });
  }
  if (state.hover && state.hover !== state.selected && bs.includes(state.hover)) drawHover(ctx, v, state.hover);
  if (state.selected) drawSelected(ctx, v);
  drawLegend();
}

function drawHover(ctx, v, b) {
  // a dark casing under a bright line, so the route stands out on any floor shade
  const path = () => {
    ctx.beginPath();
    b.x.forEach((x, k) => { const [px, py] = worldToPx(x, b.y[k]); k ? ctx.lineTo(px, py) : ctx.moveTo(px, py); });
  };
  ctx.globalAlpha = 1;
  ctx.lineJoin = 'round'; ctx.lineCap = 'round';
  ctx.strokeStyle = 'rgba(10, 12, 16, 0.85)'; ctx.lineWidth = 5.5 / v.scale; path(); ctx.stroke();
  ctx.strokeStyle = b.episode.found ? css('--found') : css('--missed'); ctx.lineWidth = 3 / v.scale; path(); ctx.stroke();
  const [sx, sy] = worldToPx(b.x[0], b.y[0]);
  ctx.fillStyle = css('--start'); ctx.beginPath(); ctx.arc(sx, sy, 4.5 / v.scale, 0, 7); ctx.fill();
}

function drawSelected(ctx, v) {
  const b = state.selected, i = state.play.i;
  ctx.globalAlpha = 1;
  ctx.lineJoin = 'round'; ctx.lineCap = 'round';
  // the whole route faint, the part walked so far bright
  ctx.strokeStyle = '#ffffff'; ctx.lineWidth = 3.2 / v.scale; ctx.globalAlpha = 0.25;
  ctx.beginPath();
  b.x.forEach((x, k) => { const [px, py] = worldToPx(x, b.y[k]); k ? ctx.lineTo(px, py) : ctx.moveTo(px, py); });
  ctx.stroke();
  ctx.globalAlpha = 1;
  ctx.strokeStyle = b.episode.found ? css('--found') : css('--missed'); ctx.lineWidth = 3.2 / v.scale;
  ctx.beginPath();
  for (let k = 0; k <= i; k++) { const [px, py] = worldToPx(b.x[k], b.y[k]); k ? ctx.lineTo(px, py) : ctx.moveTo(px, py); }
  ctx.stroke();
  const o = b.episode.object_xyz;
  if (o && o[0] != null) {
    const [ox, oy] = worldToPx(o[0], o[1]), r = 6 / v.scale;
    ctx.strokeStyle = b.episode.found ? css('--obj-found') : css('--obj-missed'); ctx.lineWidth = 2.5 / v.scale;
    ctx.strokeRect(ox - r, oy - r, 2 * r, 2 * r);
  }
  // the seat: body heading (arrow) and camera direction (cone)
  const [px, py] = worldToPx(b.x[i], b.y[i]);
  const yaw = b.yaw ? b.yaw[i] : 0, cam = yaw + (b.cam ? b.cam[i] : 0);
  // world yaw: 0 = +x (east), counter-clockwise; the image's y points down (north up), so negate the angle
  const s = 1 / v.scale;
  ctx.fillStyle = 'rgba(255, 230, 120, 0.22)';
  ctx.beginPath(); ctx.moveTo(px, py);
  ctx.arc(px, py, 26 * s, -cam - 0.5, -cam + 0.5);
  ctx.closePath(); ctx.fill();
  ctx.fillStyle = '#ffe680'; ctx.strokeStyle = '#1b1f26'; ctx.lineWidth = 1.5 * s;
  ctx.beginPath();
  ctx.moveTo(px + Math.cos(-yaw) * 9 * s, py + Math.sin(-yaw) * 9 * s);
  ctx.lineTo(px + Math.cos(-yaw + 2.5) * 6 * s, py + Math.sin(-yaw + 2.5) * 6 * s);
  ctx.lineTo(px + Math.cos(-yaw - 2.5) * 6 * s, py + Math.sin(-yaw - 2.5) * 6 * s);
  ctx.closePath(); ctx.fill(); ctx.stroke();
}

function drawLegend() {
  const items = [];
  if ($('lRoutes').checked) {
    if (state.colourBy === 'outcome') items.push(['line', css('--found'), 'found'], ['line', css('--missed'), 'not found']);
    else if (state.colourBy === 'class') items.push(['line', '#ccc', 'route, by class (see the chips)']);
    else items.push(['line', '#ccc', 'route, one colour per episode']);
  }
  if ($('lStarts').checked) items.push(['dot', css('--start'), 'start']);
  if ($('lObjects').checked) items.push(['box', css('--obj-found'), 'object found'], ['box', css('--obj-missed'), 'object missed']);
  if ($('lEnds').checked) items.push(['x', '#f2f2f2', 'end']);
  if ($('lPins').checked) items.push(['dot', '#ff3b6b', 'wall contact (bright: stopped)']);
  if ($('lHeat').checked) items.push(['line', 'linear-gradient(90deg, rgb(88,15,109), rgb(243,120,25), rgb(252,255,164))', 'time spent (log scale)']);
  if (state.bg?.water) items.push(['dot', 'rgb(52,128,214)', 'water']);
  if (state.bg?.lava) items.push(['dot', 'rgb(255,96,24)', 'lava']);
  $('legend').innerHTML = items.map(([k, c, t]) =>
    `<span><i class="${k === 'dot' ? 'dot' : k === 'box' ? 'box' : ''}" style="${k === 'box' ? 'border-color' : 'background'}:${c}"></i>${t}</span>`).join('')
    + `<span style="opacity:.7">floor shaded by height · north up</span>`;
}

// ---- replay ---------------------------------------------------------------------------------------------------------

function selectEpisode(b) {
  stopPlay();
  state.selected = b;
  state.play.i = 0;
  const e = b.episode;
  $('player').hidden = false;
  $('ptitle').textContent = `#${b.seed} · ${e.class}${e.race ? ' (' + e.race + ')' : ''}${e.level ? ' L' + e.level : ''}` +
    ` · ${e.room || 'room ?'}${e.object ? ' · ' + e.object : ''} · ` +
    (e.found ? `found in ${Math.round(e.find_seconds || 0)} s` : 'not found') +
    (e.pin_events ? ` · ${e.pin_events} pins` : '');
  $('scrub').max = b.x.length - 1;
  $('scrub').value = 0;
  zoomToRoute(b);
  renderList();
  writeHash();
  showMedia(b);
  updatePlayer();
}

// ---- the bot's view: the evaluation video's frame for the replay's decision -----------------------------------------

const frameImages = new Map();               // url -> Image, the frames already asked for
function frameUrl(b, i) {
  const k = Math.max(0, Math.min(b.video.frames - 1, i));
  return `/api/frame?run=${encodeURIComponent(state.run)}&steps=${state.steps}&file=${encodeURIComponent(b.video.file)}&i=${k}`;
}
function preload(url) {
  let im = frameImages.get(url);
  if (!im) {
    im = new Image();
    im.src = url;
    frameImages.set(url, im);
    if (frameImages.size > 1500) frameImages.delete(frameImages.keys().next().value);
  }
  return im;
}
function showMedia(b) {
  const box = $('media');
  box.hidden = false;
  $('mframe').hidden = !b.video;
  $('mopen').hidden = !b.video;
  frameImages.clear();
  if (b.video) {
    $('mtitle').textContent = "The bot's view";
    $('mopen').href = b.video.url;
    $('mnote').textContent = `${b.video.frames} frames at ${b.video.fps} per second, one per decision` +
      (b.video.dropped ? ` · ${b.video.dropped} dropped` : '') + (b.video.truncated ? ' · cut short' : '');
    return;
  }
  $('mtitle').textContent = 'No video of this route';
  const filmed = state.blocks.filter((x) => x.video && x.episode.map === state.map);
  const note = $('mnote');
  if (!state.videoCount) {
    note.textContent = state.policy === 'learner'
      ? 'This evaluation has no videos.'
      : 'Videos are filmed of the learner evaluation only (policy "learner").';
    return;
  }
  note.textContent = `The sim films ${state.videoCount} sampled episodes of each evaluation` +
    (filmed.length ? '; on this map:' : '; none on this map.');
  for (const x of filmed) {
    const btn = document.createElement('button');
    btn.textContent = `#${x.seed} ${x.episode.class}`;
    btn.onclick = () => selectEpisode(x);
    note.appendChild(btn);
  }
}
function updateFrame(b, i) {
  if (!b.video) return;
  const url = frameUrl(b, i), im = preload(url), img = $('mframe');
  const show = () => { if (state.selected === b && state.play.i === i) img.src = url; };
  if (im.complete && im.naturalWidth) show(); else im.addEventListener('load', show, { once: true });
  // keep the next stretch of frames coming while it plays
  const ahead = state.play.playing ? Math.ceil(24 * Number($('speed').value) / 4) : 4;
  for (let k = 1; k <= ahead; k++) preload(frameUrl(b, i + k));
}
function zoomToRoute(b) {
  const c = $('canvas');
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  const o = b.episode.object_xyz;
  const pts = b.x.map((x, k) => worldToPx(x, b.y[k]));
  if (o && o[0] != null) pts.push(worldToPx(o[0], o[1]));
  for (const [px, py] of pts) { x0 = Math.min(x0, px); y0 = Math.min(y0, py); x1 = Math.max(x1, px); y1 = Math.max(y1, py); }
  const pad = 40, w = c.clientWidth, h = c.clientHeight - 110;       // keep clear of the player panel
  const s = Math.min(8, Math.max(0.3, Math.min((w - 2 * pad) / Math.max(x1 - x0, 1), (h - 2 * pad) / Math.max(y1 - y0, 1))));
  state.view = { scale: s, x: w / 2 - (x0 + x1) / 2 * s, y: h / 2 - (y0 + y1) / 2 * s };
}
function updatePlayer() {
  const b = state.selected;
  if (!b) return;
  const i = state.play.i;
  $('scrub').value = i;
  const t = i * state.stepSeconds, total = (b.x.length - 1) * state.stepSeconds;
  $('ptime').textContent = `${t.toFixed(1)} / ${total.toFixed(0)} s`;
  const keys = [b.fwd && b.fwd[i] ? 'forward' : null, b.strafe && b.strafe[i] ? 'strafe' : null].filter(Boolean).join(' + ') || 'none';
  let ret = 0;
  if (b.reward) for (let k = 0; k <= i; k++) ret += b.reward[k];
  $('pinfo').textContent = `decision ${i} · x ${b.x[i].toFixed(1)} y ${b.y[i].toFixed(1)} z ${b.z[i].toFixed(1)} · ` +
    `${b.moving && b.moving[i] ? 'moving' : 'stopped'}${b.wall && b.wall[i] ? ' · against a wall' : ''} · keys ${keys}` +
    `${b.flag && b.flag[i] ? ' · object in view (' + b.flag[i] + ' px)' : ''} · return so far ${ret.toFixed(2)}`;
  updateFrame(b, i);
  draw();
}
function stopPlay() { state.play.playing = false; $('play').textContent = 'Play'; }
function tick(now) {
  if (!state.play.playing || !state.selected) return;
  const dt = (now - state.play.last) / 1000;
  state.play.last = now;
  state.play.acc = (state.play.acc || 0) + dt * Number($('speed').value) / state.stepSeconds;
  if (state.play.acc >= 1) {
    const adv = Math.floor(state.play.acc);
    state.play.acc -= adv;
    state.play.i = Math.min(state.selected.x.length - 1, state.play.i + adv);
    updatePlayer();
    if (state.play.i >= state.selected.x.length - 1) { stopPlay(); return; }
  }
  requestAnimationFrame(tick);
}

// ---- input ----------------------------------------------------------------------------------------------------------

function nearestBlock(sx, sy) {
  const [px, py] = screenToPx(sx, sy);
  let best = null, bestD = (8 / state.view.scale) ** 2;
  for (const b of visibleBlocks()) {
    for (let k = 0; k < b.x.length; k += 2) {
      const [qx, qy] = worldToPx(b.x[k], b.y[k]);
      const d = (qx - px) ** 2 + (qy - py) ** 2;
      if (d < bestD) { bestD = d; best = b; }
    }
  }
  return best;
}

function wire() {
  const c = $('canvas');
  let drag = null;
  c.addEventListener('pointerdown', (e) => { drag = { x: e.clientX, y: e.clientY, vx: state.view.x, vy: state.view.y, moved: false }; c.setPointerCapture(e.pointerId); c.classList.add('drag'); });
  let hoverQueued = null;
  const tip = $('tip');
  const hoverAt = () => {
    const { x, y } = hoverQueued;
    hoverQueued = null;
    const b = state.bg && $('lRoutes').checked ? nearestBlock(x, y) : null;
    c.style.cursor = b ? 'pointer' : '';
    if (b) {
      const e = b.episode;
      tip.hidden = false;
      tip.textContent = `#${b.seed} · ${e.class}${e.room ? ' · ' + e.room : ''} · ` +
        (e.found ? `found in ${Math.round(e.find_seconds || 0)} s` : 'not found') + (b.video ? ' · ▶ video' : '');
      tip.style.left = `${Math.min(x + 14, c.clientWidth - tip.offsetWidth - 8)}px`;
      tip.style.top = `${Math.max(8, y - 34)}px`;
    } else tip.hidden = true;
    if (b !== state.hover) { state.hover = b; draw(); }
  };
  c.addEventListener('pointerleave', () => { tip.hidden = true; if (state.hover) { state.hover = null; draw(); } });
  c.addEventListener('pointermove', (e) => {
    if (!drag) {
      const r = c.getBoundingClientRect();
      if (!hoverQueued) requestAnimationFrame(hoverAt);
      hoverQueued = { x: e.clientX - r.left, y: e.clientY - r.top };
      return;
    }
    tip.hidden = true;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
    state.view.x = drag.vx + dx; state.view.y = drag.vy + dy;
    draw();
  });
  c.addEventListener('pointerup', (e) => {
    c.classList.remove('drag');
    if (drag && !drag.moved && state.bg) {
      const r = c.getBoundingClientRect();
      const b = nearestBlock(e.clientX - r.left, e.clientY - r.top);
      if (b) selectEpisode(b);
    }
    drag = null;
  });
  c.addEventListener('wheel', (e) => {
    e.preventDefault();
    const r = c.getBoundingClientRect(), sx = e.clientX - r.left, sy = e.clientY - r.top;
    zoomAt(sx, sy, Math.exp(-e.deltaY * 0.0015));
  }, { passive: false });
  const zoomCentre = (f) => zoomAt(c.clientWidth / 2, c.clientHeight / 2, f);
  $('zin').onclick = () => zoomCentre(1.4);
  $('zout').onclick = () => zoomCentre(1 / 1.4);
  $('fit').onclick = fit;
  window.addEventListener('resize', () => { resizeCanvas(); draw(); });

  $('run').onchange = async () => { state.run = $('run').value; fillEvals(); await loadEvaluation(state.map); };
  $('eval').onchange = async () => { const p = state.policy; fillEvals($('eval').value, p); await loadEvaluation(state.map); };
  $('policy').onchange = async () => { state.policy = $('policy').value; await loadEvaluation(state.map); };
  for (const b of $('outcome').children) b.onclick = () => {
    state.outcome = b.dataset.v;
    for (const x of $('outcome').children) x.classList.toggle('on', x === b);
    state.heat = null; renderSummary(); renderList(); draw();
  };
  $('colourBy').onchange = () => { state.colourBy = $('colourBy').value; draw(); };
  $('seedFilter').oninput = () => { state.seed = $('seedFilter').value; state.heat = null; renderSummary(); renderList(); draw(); };
  for (const id of ['lRoutes', 'lStarts', 'lObjects', 'lEnds', 'lHeat', 'lPins']) $(id).onchange = draw;
  $('alpha').oninput = draw;
  $('bandApply').onclick = () => selectMap(state.map, true, [Number($('zlo').value), Number($('zhi').value)]);
  $('play').onclick = () => {
    if (!state.selected) return;
    if (state.play.playing) { stopPlay(); return; }
    if (state.play.i >= state.selected.x.length - 1) state.play.i = 0;
    state.play.playing = true; state.play.last = performance.now(); state.play.acc = 0;
    $('play').textContent = 'Pause';
    requestAnimationFrame(tick);
  };
  $('scrub').oninput = () => { stopPlay(); state.play.i = Number($('scrub').value); updatePlayer(); };
  $('pclose').onclick = () => {
    stopPlay(); state.selected = null; $('player').hidden = true; $('media').hidden = true; renderList(); writeHash(); draw();
  };
  $('mhide').onclick = () => {
    const min = $('media').classList.toggle('min');
    $('mhide').textContent = min ? '+' : '–';
    $('mhide').title = min ? 'Show the view' : 'Hide the view';
  };
  document.addEventListener('keydown', (e) => {
    if (!state.selected || e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
    if (e.key === ' ') { e.preventDefault(); $('play').click(); }
    if (e.key === 'ArrowRight') { stopPlay(); state.play.i = Math.min(state.selected.x.length - 1, state.play.i + 4); updatePlayer(); }
    if (e.key === 'ArrowLeft') { stopPlay(); state.play.i = Math.max(0, state.play.i - 4); updatePlayer(); }
    if (e.key === 'Escape') $('pclose').click();
  });
}
function zoomAt(sx, sy, f) {
  const v = state.view;
  const ns = Math.min(40, Math.max(0.05, v.scale * f));
  const k = ns / v.scale;
  v.x = sx - (sx - v.x) * k; v.y = sy - (sy - v.y) * k; v.scale = ns;
  draw();
}

wire();
resizeCanvas();
loadRuns().catch((e) => setStatus(e.message));
