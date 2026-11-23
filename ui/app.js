// Glassbox workbench. Vanilla JS, no external dependencies.
(() => {
  'use strict';

  /* ------------------------------ utilities ------------------------------ */
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function clear(node) {
    while (node && node.firstChild) node.removeChild(node.firstChild);
  }

  function append(parent, ...children) {
    for (const child of children) if (child) parent.appendChild(child);
    return parent;
  }

  function clamp(value, low, high) {
    return Math.min(high, Math.max(low, value));
  }

  const fmtBytes = (value) => {
    const n = Number(value) || 0;
    if (n >= 1024 ** 3) return (n / 1024 ** 3).toFixed(2) + ' GiB';
    if (n >= 1024 ** 2) return (n / 1024 ** 2).toFixed(2) + ' MiB';
    if (n >= 1024) return (n / 1024).toFixed(1) + ' KiB';
    return n.toFixed(0) + ' B';
  };

  const fmtTime = (ns) => {
    const n = Number(ns) || 0;
    if (Math.abs(n) >= 1e9) return (n / 1e9).toFixed(3) + ' s';
    if (Math.abs(n) >= 1e6) return (n / 1e6).toFixed(3) + ' ms';
    if (Math.abs(n) >= 1e3) return (n / 1e3).toFixed(2) + ' µs';
    return n.toFixed(0) + ' ns';
  };

  const fmtClock = (ns) => ((Number(ns) || 0) / 1e6).toFixed(3) + ' ms';
  const fmtNum = (value) => {
    const n = Number(value);
    if (!Number.isFinite(n)) return '—';
    return n.toLocaleString('en-US', { maximumFractionDigits: 2 });
  };
  const fmtDelta = (value) => {
    const n = Number(value) || 0;
    return (n > 0 ? '+' : '') + fmtNum(n);
  };
  const pct = (part, whole) => (whole > 0 ? ((part / whole) * 100).toFixed(1) + '%' : '0%');
  const hexOf = (value) => {
    if (typeof value === 'string' && value.startsWith('0x')) return value;
    try { return '0x' + BigInt(value).toString(16); } catch { return String(value); }
  };

  const PALETTE = ['#2dd4bf', '#60a5fa', '#a78bfa', '#f5d90a', '#fb7185', '#34d399', '#f97316', '#38bdf8', '#c084fc', '#facc15'];
  function colorFor(name, lightness = 55) {
    let hash = 2166136261;
    const text = String(name || '?');
    for (let i = 0; i < text.length; i++) {
      hash ^= text.charCodeAt(i);
      hash = Math.imul(hash, 16777619);
    }
    const hue = (hash >>> 0) % 360;
    return `hsl(${hue} 62% ${lightness}%)`;
  }

  function fitCanvas(canvas) {
    if (!canvas) return null;
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(2, Math.floor(rect.width));
    const height = Math.max(2, Math.floor(rect.height));
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.floor(width * dpr);
    canvas.height = Math.floor(height * dpr);
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, width, height };
  }

  function truncate(ctx, text, maxWidth) {
    if (maxWidth < 8) return '';
    if (ctx.measureText(text).width <= maxWidth) return text;
    let low = 0;
    let high = text.length;
    while (low < high) {
      const mid = Math.ceil((low + high) / 2);
      if (ctx.measureText(text.slice(0, mid) + '…').width <= maxWidth) low = mid;
      else high = mid - 1;
    }
    return text.slice(0, low) + '…';
  }

  let toastTimer = 0;
  function toast(message, bad = false) {
    const node = $('#toast');
    node.textContent = String(message);
    node.className = 'toast' + (bad ? ' bad' : '');
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => node.classList.add('hidden'), 4200);
  }

  function setStatus(text) {
    $('#status-text').textContent = String(text);
  }

  function fail(error) {
    console.error(error);
    const message = error && error.message ? error.message : String(error);
    toast(message, true);
    setStatus('error · ' + message);
  }

  const guarded = (fn) => (...args) => Promise.resolve().then(() => fn(...args)).catch(fail);

  function threadLabel(id) {
    if (!state.meta) return 'thread ' + id;
    const found = (state.meta.threads || []).find((t) => t.id === id);
    return found ? found.name : 'thread ' + id;
  }

  function wallClock(ts) {
    const meta = state.meta;
    if (!meta || !meta.wall_start_ns) return '';
    const date = new Date((Number(meta.wall_start_ns) + Number(ts || 0)) / 1e6);
    if (Number.isNaN(date.getTime())) return '';
    return date.toISOString().replace('T', ' ').replace('Z', ' UTC');
  }

  function fieldSummary(fields, max = 160) {
    if (!fields) return '';
    const parts = [];
    for (const [key, value] of Object.entries(fields)) {
      if (key === 'level') continue;
      let text = value;
      if (key === 'frames' && typeof value === 'string') {
        text = value.split('\x1f').slice(0, 6).join(' › ');
      }
      if (typeof text === 'string' && text.length > 64) text = text.slice(0, 61) + '…';
      parts.push(`${key}=${text}`);
      if (parts.join(' ').length > max) break;
    }
    return parts.join(' · ');
  }

  /* ------------------------------- state -------------------------------- */
  const state = {
    token: '',
    meta: null,
    traceEnd: 1,
    at: 0,
    window: { from: 0, to: 1 },
    a: null,
    b: null,
    view: 'overview',
    timeline: null,
    heap: null,
    heapGraph: null,
    heapGraphMode: false,
    selectedSite: '',
    selectedObject: null,
    flame: null,
    flameMode: 'samples',
    flameZoom: [],
    flameSearch: '',
    spansActive: null,
    spansWindow: null,
    logs: null,
    logLevel: 0,
    logQuery: '',
    stateEntries: null,
    diff: null,
    query: null,
    leaks: null,
    counters: null,
    liveSource: null,
    liveCount: 0,
    traces: [],
    playing: false,
    playTimer: 0,
    loadSeq: 0,
    drag: null,
    hoverRow: null,
  };

  /* ------------------------------- API ---------------------------------- */
  async function api(path, params = {}, options = {}) {
    const url = new URL(path, location.origin);
    for (const [key, value] of Object.entries(params)) {
      if (value === undefined || value === null || value === '') continue;
      url.searchParams.set(key, value);
    }
    const init = {
      method: options.method || 'GET',
      cache: 'no-store',
      headers: { 'X-Glassbox-Token': state.token },
    };
    if (options.body !== undefined) {
      init.body = options.body;
      init.headers['Content-Type'] = 'application/json';
    }
    const response = await fetch(url, init);
    const text = await response.text();
    let data = null;
    try {
      data = text ? JSON.parse(text) : {};
    } catch {
      throw new Error(`invalid JSON from ${path} (${response.status})`);
    }
    if (!response.ok) {
      const error = new Error((data && data.error) || response.statusText || `HTTP ${response.status}`);
      error.status = response.status;
      error.data = data;
      throw error;
    }
    return data;
  }

  /* ------------------------------ token --------------------------------- */
  function initToken() {
    const params = new URLSearchParams(location.search);
    const queryToken = params.get('t');
    if (queryToken) {
      sessionStorage.setItem('gbx_token', queryToken);
      params.delete('t');
      const suffix = params.toString();
      history.replaceState(null, '', location.pathname + (suffix ? '?' + suffix : '') + location.hash);
    }
    state.token = sessionStorage.getItem('gbx_token') || '';
    state.linkAt = params.get('at');
    state.linkA = params.get('a');
    state.linkB = params.get('b');
    $('#status-token').textContent = state.token ? 'tok ' + state.token.slice(0, 6) + '…' : 'no token';
  }

  function resolveMoment(text) {
    if (text === null || text === undefined || text === '') return null;
    const value = Number(text);
    if (!Number.isFinite(value) || value < 0) return null;
    if (value <= 1) return Math.round(value * state.traceEnd);
    return Math.min(state.traceEnd, Math.round(value));
  }

  /* ------------------------------ views --------------------------------- */
  const VIEW_NAMES = new Set(['overview', 'flame', 'heap', 'spans', 'logs', 'state', 'diff', 'query']);

  function switchView(name, refresh = true, updateHash = true) {
    if (!VIEW_NAMES.has(name)) return;
    state.view = name;
    if (updateHash && location.hash !== '#' + name) {
      history.replaceState(null, '', '#' + name);
    }
    $$('#nav button').forEach((button) => button.classList.toggle('active', button.dataset.view === name));
    $$('.view').forEach((view) => view.classList.toggle('active', view.id === 'view-' + name));
    if (name === 'query' && refresh) {
      setTimeout(() => { const input = $('#query-input'); if (input) input.focus(); }, 30);
    }
    if (refresh) refreshCurrentView();
    setStatus(`${state.meta ? state.meta.name : 'no trace'} · ${name} · ${fmtTime(state.at)}`);
  }

  function refreshCurrentView() {
    if (!state.meta) return;
    switch (state.view) {
      case 'overview': loadOverview(); break;
      case 'flame': loadFlame(); break;
      case 'heap': loadHeap(); break;
      case 'spans': loadSpans(); break;
      case 'logs': loadLogs(); break;
      case 'state': loadState(); break;
      case 'diff': loadDiff(); break;
      case 'query': break;
      default: break;
    }
  }

  /* ----------------------------- timeline ------------------------------- */
  function timelineTime(tsFromX, x) {
    const tl = state.timeline;
    if (!tl) return state.at;
    const width = tl.canvasWidth || 1;
    const ratio = clamp(x / width, 0, 1);
    return Math.round(tl.begin + ratio * Math.max(1, tl.end - tl.begin));
  }

  function drawTimeline() {
    const canvas = $('#timeline');
    const fitted = fitCanvas(canvas);
    if (!fitted) return;
    const { ctx, width, height } = fitted;
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#070b10';
    ctx.fillRect(0, 0, width, height);
    const tl = state.timeline;
    if (!tl) {
      ctx.fillStyle = '#54657a';
      ctx.font = '11px ui-monospace, monospace';
      ctx.fillText('No timeline data', 12, 22);
      return;
    }
    tl.canvasWidth = width;
    const span = Math.max(1, tl.end - tl.begin);
    const xOf = (ts) => 1 + ((ts - tl.begin) / span) * (width - 2);
    const buckets = tl.buckets || [];
    const spanH = height * 0.52;
    const allocTop = spanH + 8;
    const allocH = Math.max(16, height * 0.2);
    const bottomY = allocTop + allocH + 8;

    ctx.strokeStyle = '#15202b';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = (height - 18) * (i / 4) + 4;
      ctx.beginPath();
      ctx.moveTo(0, y + 0.5);
      ctx.lineTo(width, y + 0.5);
      ctx.stroke();
    }

    const maxSpan = Math.max(1, tl.max_span_ns || 0);
    const maxAlloc = Math.max(1, tl.max_alloc_bytes || 0);

    // Span time area.
    if (buckets.length > 0) {
      const gradient = ctx.createLinearGradient(0, 0, 0, spanH);
      gradient.addColorStop(0, '#2dd4bf88');
      gradient.addColorStop(1, '#0d5c5333');
      ctx.beginPath();
      ctx.moveTo(xOf(buckets[0].begin), spanH);
      for (const bucket of buckets) {
        const x = xOf((bucket.begin + bucket.end) / 2);
        const y = spanH - Math.min(1, bucket.span_ns / maxSpan) * (spanH - 6);
        ctx.lineTo(x, y);
      }
      ctx.lineTo(xOf(buckets[buckets.length - 1].end), spanH);
      ctx.closePath();
      ctx.fillStyle = gradient;
      ctx.fill();
      ctx.strokeStyle = '#5eead4aa';
      ctx.lineWidth = 1.2;
      ctx.stroke();

      // Alloc bytes bars.
      for (const bucket of buckets) {
        if (!bucket.alloc_bytes) continue;
        const x = xOf(bucket.begin);
        const next = xOf(bucket.end);
        const barW = Math.max(1, next - x - 1);
        const h = Math.max(1, (bucket.alloc_bytes / maxAlloc) * (allocH - 2));
        ctx.fillStyle = '#f5d90a99';
        ctx.fillRect(x, allocTop + allocH - h, barW, h);
      }

      // Error ticks and sample dots.
      for (const bucket of buckets) {
        const x = xOf((bucket.begin + bucket.end) / 2);
        if (bucket.error_count > 0) {
          ctx.fillStyle = '#fb7185';
          ctx.fillRect(x - 1, bottomY, 2, Math.min(12, 3 + bucket.error_count * 2));
        }
        if (bucket.sample_count > 0) {
          ctx.fillStyle = '#a78bfa';
          ctx.fillRect(x - 0.5, bottomY + 14, 2, 3);
        }
      }
    }

    // Selected window shading and A/B markers.
    const xw0 = xOf(state.window.from);
    const xw1 = xOf(state.window.to);
    ctx.fillStyle = 'rgba(3, 6, 10, 0.28)';
    if (xw0 > 0) ctx.fillRect(0, 0, xw0, height);
    if (xw1 < width) ctx.fillRect(xw1, 0, width - xw1, height);
    ctx.strokeStyle = '#31465b';
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(xw0, 0); ctx.lineTo(xw0, height);
    ctx.moveTo(xw1, 0); ctx.lineTo(xw1, height);
    ctx.stroke();
    ctx.setLineDash([]);

    const marker = (value, color, label) => {
      if (value === null || value === undefined) return;
      if (value < tl.begin || value > tl.end) return;
      const x = xOf(value);
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(x, 0); ctx.lineTo(x, height);
      ctx.stroke();
      ctx.fillStyle = color;
      ctx.font = 'bold 9px ui-monospace, monospace';
      ctx.fillText(label, clamp(x + 3, 2, width - 12), 11);
    };
    marker(state.a, '#f5d90a', 'A');
    marker(state.b, '#60a5fa', 'B');

    const xAt = xOf(state.at);
    ctx.strokeStyle = '#eaf6f4';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(xAt, 0); ctx.lineTo(xAt, height);
    ctx.stroke();
    ctx.fillStyle = '#eaf6f4';
    ctx.beginPath();
    ctx.arc(xAt, 3, 3, 0, Math.PI * 2);
    ctx.fill();

    if (state.drag && state.drag.shift) {
      const x0 = state.drag.x0;
      const x1 = state.drag.x1 === undefined ? x0 : state.drag.x1;
      ctx.fillStyle = '#5eead422';
      ctx.fillRect(Math.min(x0, x1), 0, Math.abs(x1 - x0), height);
    }

    // Time axis labels.
    ctx.fillStyle = '#66788c';
    ctx.font = '9px ui-monospace, monospace';
    for (let i = 0; i <= 4; i++) {
      const ts = tl.begin + span * (i / 4);
      const label = fmtTime(Math.round(ts));
      const metric = ctx.measureText(label).width;
      const x = clamp(1 + ((width - 2) * i) / 4, 2, width - metric - 2);
      ctx.fillText(label, x, height - 4);
    }
  }

  function setAt(ts, options = {}) {
    const from = state.window.from;
    const to = state.window.to;
    state.at = clamp(Math.round(Number(ts) || 0), from, to);
    $('#clock').textContent = fmtClock(state.at);
    $('#clock-wall').textContent = wallClock(state.at);
    const slider = $('#heap-at');
    if (slider && document.activeElement !== slider && state.traceEnd > 0) {
      slider.value = String((state.at / state.traceEnd) * 100);
      $('#heap-at-label').textContent = fmtTime(state.at);
    }
    drawTimeline();
    if (options.refresh !== false) scheduleTimeRefresh();
  }

  let timeRefreshTimer = 0;
  let windowRefreshTimer = 0;

  function scheduleTimeRefresh() {
    window.clearTimeout(timeRefreshTimer);
    timeRefreshTimer = window.setTimeout(guarded(async () => {
      const seq = ++state.loadSeq;
      if (state.view === 'overview') await loadOverview(seq);
      else if (state.view === 'heap') await loadHeap(seq);
      else if (state.view === 'spans') await loadSpans(seq);
      else if (state.view === 'state') await loadState(seq);
    }), 110);
  }

  function applyWindow(from, to, options = {}) {
    const low = 0;
    const high = Math.max(1, state.traceEnd);
    let start = clamp(Math.round(Math.min(from, to)), low, high);
    let end = clamp(Math.round(Math.max(from, to)), low, high);
    if (end - start < 1000) {
      start = clamp(start - 500, low, high);
      end = clamp(end + 500, start + 1000, high);
      if (end - start < 1000) end = Math.min(high, start + 1000);
    }
    state.window = { from: start, to: end };
    state.at = clamp(state.at, start, end);
    setAt(state.at, { refresh: false });
    loadTimeline().then(() => {
      drawTimeline();
      if (options.refresh !== false) refreshCurrentView();
    }).catch(fail);
    window.clearTimeout(windowRefreshTimer);
  }

  function zoomAt(ts, factor) {
    const span = Math.max(1000, state.window.to - state.window.from);
    const target = clamp(span * factor, 1000, Math.max(1000, state.traceEnd));
    const ratio = (ts - state.window.from) / span;
    let from = ts - ratio * target;
    let to = from + target;
    if (from < 0) { to -= from; from = 0; }
    if (to > state.traceEnd) { from -= to - state.traceEnd; to = state.traceEnd; }
    applyWindow(Math.max(0, from), Math.min(state.traceEnd, to));
  }

  function resetWindow() {
    applyWindow(0, state.traceEnd);
  }

  async function loadTimeline() {
    if (!state.meta) return;
    const data = await api('/api/timeline', {
      from: state.window.from,
      to: state.window.to,
      buckets: 200,
    });
    state.timeline = { ...data, canvasWidth: $('#timeline').getBoundingClientRect().width || 1 };
    drawTimeline();
  }

  /* ----------------------------- overview ------------------------------- */
  function addCard(parent, label, value, sub, kind = '') {
    const card = el('div', 'card ' + kind);
    append(card, el('div', 'label', label), el('div', 'value', value));
    if (sub) card.appendChild(el('div', 'sub', sub));
    parent.appendChild(card);
    return card;
  }

  function addStackRow(parent, title, detail, time, onClick, depth = 0) {
    const row = el('div', 'row-item clickable');
    if (depth > 0) row.style.paddingLeft = 7 + Math.min(depth, 6) * 12 + 'px';
    row.appendChild(el('span', 'name', title));
    if (detail) row.appendChild(el('span', 'muted small', detail));
    if (time) row.appendChild(el('span', 'time', time));
    if (onClick) row.addEventListener('click', onClick);
    parent.appendChild(row);
    return row;
  }

  function emptyMessage(parent, text) {
    parent.appendChild(el('div', 'empty', text));
  }

  async function loadOverview(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const at = state.at;
    const from = state.window.from;
    const to = state.window.to;
    const [heap, spans, logs, events, leaks, counters] = await Promise.all([
      api('/api/heap', { at, limit: 1 }),
      api('/api/spans', { at }),
      api('/api/logs', { from: 0, to: at, limit: 7, newest: 1 }),
      api('/api/events', { from: 0, to: at, limit: 12, newest: 1 }),
      api('/api/leaks', { limit: 6 }),
      api('/api/counters', { points: 64 }),
    ]);
    if (seq !== state.loadSeq) return;
    state.leaks = leaks;
    state.counters = counters;
    state.heap = heap;

    const cards = $('#overview-cards');
    clear(cards);
    addCard(cards, 'Live heap at T', fmtBytes(heap.total_bytes), `${fmtNum(heap.object_count)} objects · peak ${fmtBytes(heap.peak_bytes)}`);
    const busy = (state.timeline?.buckets || []).reduce((sum, b) => sum + (b.span_ns || 0), 0);
    addCard(cards, 'Span time (sum)', fmtTime(busy), 'nested and parallel spans included', 'good');
    addCard(cards, 'Allocations', fmtNum(heap.alloc_count), `${fmtNum(heap.free_count)} frees · ${fmtNum(heap.unknown_frees)} unknown`);
    addCard(cards, 'Errors in trace', fmtNum(state.timeline?.total_errors || 0), `${fmtNum(state.timeline?.total_logs || 0)} log records`, (state.timeline?.total_errors || 0) > 0 ? 'danger' : '');
    addCard(cards, 'Samples', fmtNum(state.timeline?.total_samples || 0), 'stack samples in window');
    const leakBytes = (leaks.leaks || []).reduce((sum, leak) => sum + leak.bytes, 0);
    addCard(cards, 'Leak candidates', fmtBytes(leakBytes), `${leaks.count || 0} sites`, 'warn');

    // Counters with sparklines.
    const charts = $('#overview-counters');
    clear(charts);
    if (!counters.series || counters.series.length === 0) {
      emptyMessage(charts, 'No counters recorded.');
    } else {
      for (const series of counters.series.slice(0, 8)) {
        const chart = el('div', 'chart');
        const name = el('div', 'name');
        name.appendChild(document.createTextNode(series.name));
        if (series.unit) name.appendChild(el('span', 'unit', ' ' + series.unit));
        chart.appendChild(name);
        const canvas = el('canvas');
        chart.appendChild(canvas);
        chart.appendChild(el('div', 'value', fmtNum(series.last)));
        chart.title = `${series.name}: min ${fmtNum(series.min)} · max ${fmtNum(series.max)} · last ${fmtNum(series.last)}`;
        chart.addEventListener('click', () => showInspector(`counter · ${series.name}`, [
          { title: 'summary', pairs: [['kind', series.kind], ['unit', series.unit || '—'], ['min', fmtNum(series.min)], ['max', fmtNum(series.max)], ['last', fmtNum(series.last)], ['points', series.points.length]] },
          { title: 'recent points', pre: series.points.slice(-16).map(([ts, v]) => `${fmtTime(ts)}  ${fmtNum(v)}`).join('\n') },
        ]));
        charts.appendChild(chart);
        drawSparkline(canvas, series.points, colorFor(series.name, 60));
      }
    }

    // Active spans.
    const spanBox = $('#overview-spans');
    clear(spanBox);
    $('#active-span-count').textContent = String(spans.count || 0);
    if (!spans.spans || spans.spans.length === 0) {
      emptyMessage(spanBox, 'No spans active at this moment.');
    } else {
      for (const span of spans.spans.slice(0, 18)) {
        addStackRow(spanBox, span.name, span.kind || '', fmtTime(Math.max(0, at - span.begin_ts)),
          () => openSpan(span.id), span.depth || 0);
      }
    }

    // Logs.
    const logBox = $('#overview-logs');
    clear(logBox);
    if (!logs.records || logs.records.length === 0) {
      emptyMessage(logBox, 'No logs yet.');
    } else {
      for (const record of logs.records) {
        const level = normalizeLevel(record.fields?.level);
        const row = el('div', 'row-item clickable');
        row.appendChild(el('span', 'level ' + level, level));
        row.appendChild(el('span', 'name', record.fields?.msg || '(empty)'));
        row.appendChild(el('span', 'time', fmtTime(record.ts)));
        row.addEventListener('click', () => openEvent(record.id));
        logBox.appendChild(row);
      }
    }

    // Leaks.
    const leakBody = $('#overview-leaks tbody');
    clear(leakBody);
    if (!leaks.leaks || leaks.leaks.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No live allocations at the end of the trace.');
      td.colSpan = 4;
      tr.appendChild(td);
      leakBody.appendChild(tr);
    } else {
      for (const leak of leaks.leaks.slice(0, 6)) {
        const tr = el('tr', 'clickable');
        tr.appendChild(el('td', 'mono', leak.site));
        tr.appendChild(el('td', 'dim', leak.class));
        tr.appendChild(el('td', 'num', fmtBytes(leak.bytes)));
        tr.appendChild(el('td', 'num', fmtNum(leak.count)));
        tr.addEventListener('click', () => selectSite(leak.site, leak.class));
        leakBody.appendChild(tr);
      }
    }

    // Event feed.
    const feed = $('#overview-events');
    clear(feed);
    if (!events.records || events.records.length === 0) {
      emptyMessage(feed, 'No events in this window.');
    } else {
      for (const record of events.records) {
        feed.appendChild(eventRow(record));
      }
    }

    $('#status-heap').textContent = `heap ${fmtBytes(heap.total_bytes)} @ ${fmtTime(at)}`;
  }

  function drawSparkline(canvas, points, color) {
    const fitted = fitCanvas(canvas);
    if (!fitted) return;
    const { ctx, width, height } = fitted;
    if (!points || points.length === 0) return;
    const values = points.map((p) => p[1]);
    let min = Math.min(...values);
    let max = Math.max(...values);
    if (max === min) { max += 1; min -= 1; }
    const first = points[0][0];
    const last = points[points.length - 1][0];
    const span = Math.max(1, last - first);
    ctx.beginPath();
    points.forEach(([ts, value], index) => {
      const x = ((ts - first) / span) * (width - 2) + 1;
      const y = height - 3 - ((value - min) / (max - min)) * (height - 6);
      if (index === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.3;
    ctx.stroke();
    ctx.lineTo(width - 1, height);
    ctx.lineTo(1, height);
    ctx.closePath();
    ctx.fillStyle = color + '22';
    ctx.fill();
  }

  function normalizeLevel(level) {
    const names = ['trace', 'debug', 'info', 'warn', 'error', 'fatal'];
    const index = Number(level);
    return names[Number.isFinite(index) ? clamp(index, 0, 5) : 2];
  }

  function eventRow(record) {
    const row = el('div', 'event-row');
    row.appendChild(el('div', 't', fmtTime(record.ts)));
    const type = el('div', 'k', record.type);
    row.appendChild(type);
    row.appendChild(el('div', 'th', record.thread_name || threadLabel(record.thread)));
    row.appendChild(el('div', 'f', fieldSummary(record.fields)));
    row.addEventListener('click', () => openEvent(record.id));
    return row;
  }

  /* ------------------------------- flame -------------------------------- */
  async function loadFlame(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const data = await api('/api/flame', {
      from: state.window.from,
      to: state.window.to,
      mode: state.flameMode,
      nodes: 12000,
    });
    if (seq !== state.loadSeq) return;
    state.flame = data;
    state.flameZoom = [];
    renderFlame();
  }

  function currentNode() {
    let node = state.flame?.root;
    if (!node) return null;
    for (const index of state.flameZoom) {
      if (!node.children || !node.children[index]) return node;
      node = node.children[index];
    }
    return node;
  }

  function flameAmount(value) {
    return state.flame && state.flame.derived_from_spans
      ? fmtTime(value)
      : fmtNum(value) + ' samples';
  }

  function renderFlame() {
    const canvas = $('#flame-canvas');
    const fitted = fitCanvas(canvas);
    const summary = $('#flame-summary');
    const detail = $('#flame-detail');
    if (!fitted) return;
    const { ctx, width, height } = fitted;
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#070b10';
    ctx.fillRect(0, 0, width, height);
    if (!state.flame) {
      ctx.fillStyle = '#54657a';
      ctx.font = '12px ui-monospace, monospace';
      ctx.fillText('Loading flame graph…', 14, 24);
      return;
    }
    const root = currentNode();
    if (!root) return;
    state.flameRectangles = [];
    const rows = [];
    const maxRows = 30000;
    const walk = (node, depth, x0, x1, path) => {
      if (rows.length > maxRows) return;
      rows.push({ node, depth, x0, x1, path });
      if (!node.children || depth > 120) return;
      let x = x0;
      const total = node.total || 1;
      node.children.forEach((child, index) => {
        const childWidth = (x1 - x0) * (child.total / total);
        if (childWidth >= 0.4) walk(child, depth + 1, x, x + childWidth, path.concat(index));
        x += childWidth;
      });
    };
    walk(root, 0, 0, width, []);
    let maxDepth = 1;
    for (const row of rows) maxDepth = Math.max(maxDepth, row.depth + 1);
    const rowHeight = Math.max(12, Math.min(24, (height - 8) / maxDepth));
    const search = state.flameSearch.trim().toLowerCase();

    for (const row of rows) {
      const y = 4 + row.depth * rowHeight;
      if (y + rowHeight > height) continue;
      const x = row.x0;
      const w = Math.max(0, row.x1 - row.x0 - 0.6);
      if (w < 0.4) continue;
      const shade = 42 + (row.depth % 6) * 4;
      ctx.fillStyle = colorFor(row.node.name, shade);
      ctx.fillRect(x, y, w, rowHeight - 1);
      if (search && row.node.name.toLowerCase().includes(search)) {
        ctx.strokeStyle = '#f5d90a';
        ctx.lineWidth = 1.5;
        ctx.strokeRect(x + 0.75, y + 0.75, w - 1.5, rowHeight - 2.5);
      }
      if (w > 48) {
        ctx.fillStyle = '#041014';
        ctx.font = '10px ui-monospace, monospace';
        const label = truncate(ctx, row.node.name, w - 8);
        ctx.fillText(label, x + 4, y + rowHeight - 7);
        if (w > 150) {
          ctx.fillStyle = '#04101499';
          ctx.font = '9px ui-monospace, monospace';
          const value = flameAmount(row.node.self) + ' self';
          const valueWidth = ctx.measureText(value).width;
          if (valueWidth + 12 < w) ctx.fillText(value, x + w - valueWidth - 4, y + rowHeight - 7);
        }
      }
      state.flameRectangles.push({ ...row, y, width: w, height: rowHeight - 1 });
    }

    // Breadcrumbs.
    const crumbs = $('#flame-breadcrumbs');
    clear(crumbs);
    const rootButton = el('button', '', 'all');
    rootButton.addEventListener('click', () => { state.flameZoom = []; renderFlame(); });
    crumbs.appendChild(rootButton);
    let pathNode = state.flame.root;
    state.flameZoom.forEach((index, level) => {
      crumbs.appendChild(el('span', 'sep', '›'));
      pathNode = pathNode.children[index];
      const button = el('button', '', pathNode.name);
      button.addEventListener('click', () => {
        state.flameZoom = state.flameZoom.slice(0, level + 1);
        renderFlame();
      });
      crumbs.appendChild(button);
    });

    const total = state.flame.total_weight || 0;
    summary.textContent = `${state.flame.derived_from_spans ? 'derived from span durations' : 'stack samples'} · total ${flameAmount(total)} · ${fmtNum(state.flame.samples)} samples · ${fmtNum(rows.length)} frames drawn${state.flame.truncated ? ' · truncated at limit' : ''}`;
    detail.textContent = root === state.flame.root
      ? 'Click a frame to zoom in. Hover for self and inclusive time.'
      : `zoomed to ${root.name} · inclusive ${flameAmount(root.total)} · self ${flameAmount(root.self)} · ${pct(root.total, total)} of total`;
  }

  function flameHover(event) {
    if (!state.flameRectangles) return;
    const rect = $('#flame-canvas').getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    let hit = null;
    for (let i = state.flameRectangles.length - 1; i >= 0; i--) {
      const candidate = state.flameRectangles[i];
      if (x >= candidate.x0 && x <= candidate.x1 && y >= candidate.y && y <= candidate.y + candidate.height) {
        hit = candidate;
        break;
      }
    }
    const tooltip = $('#flame-tooltip');
    if (!hit) {
      tooltip.classList.add('hidden');
      return;
    }
    const root = currentNode();
    tooltip.textContent = '';
    append(tooltip,
      el('div', 'mono', hit.node.name),
      el('div', '', `inclusive ${flameAmount(hit.node.total)} · self ${flameAmount(hit.node.self)}`),
      el('div', 'muted', `${pct(hit.node.total, root?.total || 1)} of this frame · depth ${hit.depth}`));
    tooltip.style.left = Math.min(window.innerWidth - 300, event.clientX + 14) + 'px';
    tooltip.style.top = Math.min(window.innerHeight - 70, event.clientY + 12) + 'px';
    tooltip.classList.remove('hidden');
  }

  function flameClick(event) {
    if (!state.flameRectangles) return;
    const rect = $('#flame-canvas').getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    for (let i = state.flameRectangles.length - 1; i >= 0; i--) {
      const hit = state.flameRectangles[i];
      if (x >= hit.x0 && x <= hit.x1 && y >= hit.y && y <= hit.y + hit.height) {
        if (hit.node.children && hit.node.children.length > 0) {
          state.flameZoom = state.flameZoom.concat(hit.path);
          renderFlame();
        }
        return;
      }
    }
  }

  /* -------------------------------- heap -------------------------------- */
  async function loadHeap(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const heap = await api('/api/heap', { at: state.at, limit: 2000 });
    if (seq !== state.loadSeq) return;
    state.heap = heap;
    renderHeap();
    if (state.heapGraphMode) {
      const graph = await api('/api/heap/graph', { at: state.at, nodes: 4000 });
      if (seq !== state.loadSeq) return;
      state.heapGraph = graph;
      renderHeap();
    }
  }

  function renderHeap() {
    const heap = state.heap;
    const cards = $('#heap-cards');
    clear(cards);
    if (!heap) {
      emptyMessage(cards, 'Loading heap…');
      return;
    }
    addCard(cards, 'Live bytes at T', fmtBytes(heap.total_bytes), `${fmtNum(heap.object_count)} objects`);
    addCard(cards, 'Peak live', fmtBytes(heap.peak_bytes), fmtTime(heap.peak_ts || 0), 'warn');
    addCard(cards, 'Churn', fmtNum(heap.alloc_count), `${fmtNum(heap.free_count)} frees · ${fmtNum(heap.unknown_frees)} unknown`);
    addCard(cards, 'Sites', fmtNum(heap.sites?.length || 0), heap.sites_truncated ? `${heap.sites_truncated} truncated` : 'all shown');
    $('#heap-at-label').textContent = fmtTime(state.at);
    $('#heap-at').value = state.meta && state.traceEnd > 0 ? String((state.at / state.traceEnd) * 100) : '100';
    $('#heap-site-count').textContent = String(heap.sites?.length || 0);
    $('#heap-object-count').textContent = `${fmtNum(heap.object_count)} total`;

    const sitesBody = $('#heap-sites tbody');
    clear(sitesBody);
    if (!heap.sites || heap.sites.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No live allocations at this instant.');
      td.colSpan = 5;
      tr.appendChild(td);
      sitesBody.appendChild(tr);
    } else {
      for (const site of heap.sites) {
        const key = site.site + '\x1f' + site.class;
        const tr = el('tr', state.selectedSite === key ? 'selected' : '');
        tr.appendChild(el('td', 'mono', site.site || '(unknown)'));
        tr.appendChild(el('td', 'dim', site.class));
        tr.appendChild(el('td', 'num', fmtBytes(site.live_bytes)));
        tr.appendChild(el('td', 'num', fmtNum(site.live_count)));
        tr.appendChild(el('td', 'num', fmtTime(site.avg_age_ns)));
        tr.addEventListener('click', () => selectSite(site.site, site.class));
        sitesBody.appendChild(tr);
      }
    }

    const objectsBody = $('#heap-objects tbody');
    clear(objectsBody);
    let objects = heap.objects || [];
    if (state.selectedSite) {
      const [site, klass] = state.selectedSite.split('\x1f');
      objects = objects.filter((object) => object.site === site && (object.class || '') === (klass || ''));
      $('#heap-selected-site').textContent = `filter: ${site}${klass ? ' · ' + klass : ''} — ${objects.length} object${objects.length === 1 ? '' : 's'} in the top ${heap.objects?.length || 0}`;
    } else {
      $('#heap-selected-site').textContent = 'Treemap and object list show the largest live allocations.';
    }
    const visibleObjects = objects.slice(0, 400);
    if (visibleObjects.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', state.selectedSite ? 'No top objects for this site (try selecting from the treemap).' : 'No live objects.');
      td.colSpan = 5;
      tr.appendChild(td);
      objectsBody.appendChild(tr);
    } else {
      for (const object of visibleObjects) {
        const tr = el('tr');
        tr.appendChild(el('td', 'mono', hexOf(object.ptr)));
        tr.appendChild(el('td', 'num', fmtBytes(object.size)));
        tr.appendChild(el('td', 'mono dim', object.site || '—'));
        tr.appendChild(el('td', 'mono dim', object.owner === '0x0' ? 'root' : hexOf(object.owner)));
        tr.appendChild(el('td', 'num', fmtTime(object.age_ns)));
        tr.addEventListener('click', () => openObject(object));
        objectsBody.appendChild(tr);
      }
    }
    renderHeapTreemap();
  }

  function selectSite(site, klass) {
    state.selectedSite = site + '\x1f' + (klass || '');
    if (state.view !== 'heap') switchView('heap');
    else { if (state.heap) renderHeap(); else loadHeap(); }
  }

  function renderHeapTreemap() {
    const canvas = $('#heap-treemap');
    const fitted = fitCanvas(canvas);
    if (!fitted) return;
    const { ctx, width, height } = fitted;
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#070b10';
    ctx.fillRect(0, 0, width, height);
    let items = [];
    if (state.heapGraphMode && state.heapGraph?.nodes) {
      items = state.heapGraph.nodes
        .filter((node) => node.size > 0)
        .map((node) => ({
          label: node.class || node.site || hexOf(node.ptr),
          sub: `${hexOf(node.ptr)} · retained ${fmtBytes(node.retained)}`,
          bytes: node.size,
          retained: node.retained,
          node,
        }));
      items.sort((a, b) => b.bytes - a.bytes);
    } else if (state.heap?.sites) {
      items = state.heap.sites.map((site) => ({
        label: site.class || site.site || '(unknown)',
        sub: `${site.site} · ${site.live_count} objects`,
        bytes: site.live_bytes,
        site,
      }));
    }
    items = items.filter((item) => item.bytes > 0).slice(0, 80);
    state.treemapRects = [];
    if (items.length === 0) {
      ctx.fillStyle = '#54657a';
      ctx.font = '11px ui-monospace, monospace';
      ctx.fillText('No live bytes to map.', 12, 22);
      return;
    }
    const total = items.reduce((sum, item) => sum + item.bytes, 0);
    const rects = squarify(items, 2, 2, width - 4, height - 4, total);
    for (const rect of rects) {
      ctx.fillStyle = colorFor(rect.item.label, 38);
      ctx.fillRect(rect.x, rect.y, Math.max(1, rect.w - 1), Math.max(1, rect.h - 1));
      ctx.strokeStyle = '#070b10';
      ctx.strokeRect(rect.x + 0.5, rect.y + 0.5, Math.max(1, rect.w - 1), Math.max(1, rect.h - 1));
      if (rect.w > 56 && rect.h > 20) {
        ctx.fillStyle = '#dff7f2';
        ctx.font = '10px ui-monospace, monospace';
        ctx.fillText(truncate(ctx, rect.item.label, rect.w - 8), rect.x + 4, rect.y + 13);
        if (rect.h > 32) {
          ctx.fillStyle = '#bfe5df99';
          ctx.font = '9px ui-monospace, monospace';
          ctx.fillText(truncate(ctx, fmtBytes(rect.item.bytes), rect.w - 8), rect.x + 4, rect.y + 25);
        }
      }
      state.treemapRects.push(rect);
    }
  }

  function squarify(items, x, y, width, height, total) {
    if (!items.length) return [];
    if (items.length === 1) return [{ item: items[0], x, y, w: width, h: height }];
    let sum = 0;
    let split = 0;
    for (; split < items.length; split++) {
      const next = sum + items[split].bytes;
      if (next > total / 2 && split > 0) break;
      sum = next;
    }
    if (split === 0) split = 1;
    const first = items.slice(0, split);
    const rest = items.slice(split);
    const firstTotal = first.reduce((acc, item) => acc + item.bytes, 0);
    const ratio = firstTotal / total;
    if (width >= height) {
      const splitX = width * ratio;
      return [
        ...squarify(first, x, y, splitX, height, firstTotal),
        ...squarify(rest, x + splitX, y, width - splitX, height, total - firstTotal),
      ];
    }
    const splitY = height * ratio;
    return [
      ...squarify(first, x, y, width, splitY, firstTotal),
      ...squarify(rest, x, y + splitY, width, height - splitY, total - firstTotal),
    ];
  }

  function heapTreemapHover(event) {
    if (!state.treemapRects) return;
    const rect = $('#heap-treemap').getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    const hit = state.treemapRects.find((candidate) => x >= candidate.x && x <= candidate.x + candidate.w && y >= candidate.y && y <= candidate.y + candidate.h);
    const tooltip = $('#heap-tooltip');
    if (!hit) {
      tooltip.classList.add('hidden');
      return;
    }
    tooltip.textContent = '';
    append(tooltip,
      el('div', 'mono', hit.item.label),
      el('div', '', `${fmtBytes(hit.item.bytes)} live${hit.item.retained ? ' · retained ' + fmtBytes(hit.item.retained) : ''}`),
      el('div', 'muted', hit.item.sub || ''));
    tooltip.style.left = Math.min(window.innerWidth - 280, event.clientX + 14) + 'px';
    tooltip.style.top = Math.min(window.innerHeight - 80, event.clientY + 12) + 'px';
    tooltip.classList.remove('hidden');
  }

  function heapTreemapClick(event) {
    if (!state.treemapRects) return;
    const rect = $('#heap-treemap').getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    const hit = state.treemapRects.find((candidate) => x >= candidate.x && x <= candidate.x + candidate.w && y >= candidate.y && y <= candidate.y + candidate.h);
    if (!hit) return;
    if (hit.item.node) {
      openGraphNode(hit.item.node);
    } else if (hit.item.site) {
      selectSite(hit.item.site.site, hit.item.site.class);
    }
  }

  /* -------------------------------- spans ------------------------------- */
  async function loadSpans(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const [active, tree] = await Promise.all([
      api('/api/spans', { at: state.at }),
      api('/api/spans', { from: state.window.from, to: state.window.to, limit: 2000 }),
    ]);
    if (seq !== state.loadSeq) return;
    state.spansActive = active;
    state.spansWindow = tree;
    renderSpans();
  }

  function renderSpans() {
    const active = state.spansActive;
    const tree = state.spansWindow;
    if (!tree) return;
    $('#spans-count').textContent = `${tree.returned || 0} spans · ${tree.unmatched_ends || 0} unmatched ends`;
    const activeOnly = $('#spans-active-only').checked;
    const treeBox = $('#spans-tree');
    clear(treeBox);
    const activeSpans = active?.spans || [];
    if (activeSpans.length === 0) {
      emptyMessage(treeBox, 'No spans active at this moment. Scrub earlier on the timeline to see the tree.');
    } else {
      for (const span of activeSpans) {
        addStackRow(treeBox, span.name, span.kind || '', `id ${span.id}`, () => openSpan(span.id), span.depth || 0);
      }
    }

    const rows = (tree.spans || []).filter((span) => !activeOnly || activeSpans.some((activeSpan) => activeSpan.id === span.id));
    rows.sort((a, b) => {
      const da = (a.end_ts || a.begin_ts) - a.begin_ts;
      const db = (b.end_ts || b.begin_ts) - b.begin_ts;
      return db - da;
    });
    const tableBody = $('#spans-table tbody');
    clear(tableBody);
    if (rows.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No spans in this window.');
      td.colSpan = 6;
      tr.appendChild(td);
      tableBody.appendChild(tr);
    } else {
      for (const span of rows.slice(0, 300)) {
        const tr = el('tr');
        tr.style.paddingLeft = (span.depth || 0) * 4 + 'px';
        tr.appendChild(el('td', 'mono', '  '.repeat(Math.min(span.depth || 0, 8)) + span.name));
        tr.appendChild(el('td', 'dim', span.kind || ''));
        tr.appendChild(el('td', 'mono', threadLabel(span.thread)));
        tr.appendChild(el('td', 'num', fmtTime(span.begin_ts)));
        tr.appendChild(el('td', 'num', span.closed ? fmtTime(span.end_ts - span.begin_ts) : 'open'));
        tr.appendChild(el('td', 'num', span.depth || 0));
        tr.addEventListener('click', () => openSpan(span.id));
        tableBody.appendChild(tr);
      }
    }
    drawSpansCanvas();
  }

  function drawSpansCanvas() {
    const canvas = $('#spans-canvas');
    const fitted = fitCanvas(canvas);
    if (!fitted) return;
    const { ctx, width, height } = fitted;
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#070b10';
    ctx.fillRect(0, 0, width, height);
    const tree = state.spansWindow;
    if (!tree || !tree.spans || tree.spans.length === 0) {
      ctx.fillStyle = '#54657a';
      ctx.font = '11px ui-monospace, monospace';
      ctx.fillText('No spans in this window.', 12, 22);
      return;
    }
    const from = tree.from;
    const to = tree.to;
    const span = Math.max(1, to - from);
    const threads = [...new Set(tree.spans.map((item) => item.thread))].sort((a, b) => a - b);
    const lanes = threads.length;
    const laneHeight = Math.max(18, Math.min(48, (height - 24) / Math.max(1, lanes)));
    const xOf = (ts) => 96 + ((clamp(ts, from, to) - from) / span) * (width - 104);
    ctx.font = '9px ui-monospace, monospace';
    threads.forEach((thread, lane) => {
      const laneY = 20 + lane * laneHeight;
      ctx.fillStyle = '#0c141d';
      ctx.fillRect(0, laneY, width, laneHeight - 2);
      ctx.fillStyle = '#6b7f93';
      ctx.fillText(truncate(ctx, threadLabel(thread), 86), 4, laneY + 12);
    });
    const limit = 900;
    tree.spans.slice(0, limit).forEach((record) => {
      const lane = threads.indexOf(record.thread);
      if (lane < 0) return;
      const laneY = 20 + lane * laneHeight;
      const depth = Math.min(record.depth || 0, 6);
      const y = laneY + 2 + depth * 3;
      const x0 = xOf(record.begin_ts);
      const end = record.closed ? record.end_ts : Math.min(to, state.traceEnd);
      const x1 = Math.max(x0 + 1, xOf(end));
      ctx.fillStyle = colorFor(record.name, 42 + depth * 3);
      ctx.fillRect(x0, y, Math.max(1, x1 - x0), Math.max(6, laneHeight - 12 - depth * 3));
      if (x1 - x0 > 46) {
        ctx.fillStyle = '#e6fffa';
        ctx.font = '9px ui-monospace, monospace';
        ctx.fillText(truncate(ctx, record.name, x1 - x0 - 6), x0 + 3, y + Math.max(7, laneHeight - 14 - depth * 3));
      }
    });
    ctx.fillStyle = '#66788c';
    for (let i = 0; i <= 4; i++) {
      const x = 96 + ((width - 104) * i) / 4;
      const ts = from + span * (i / 4);
      ctx.fillText(fmtTime(ts), Math.min(x, width - 46), height - 4);
      ctx.strokeStyle = '#15202b';
      ctx.beginPath();
      ctx.moveTo(x, 16); ctx.lineTo(x, height - 12);
      ctx.stroke();
    }
  }

  /* -------------------------------- logs -------------------------------- */
  async function loadLogs(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const data = await api('/api/logs', {
      from: state.window.from,
      to: state.window.to,
      limit: 500,
      level: state.logLevel,
      q: state.logQuery.trim() || undefined,
    });
    if (seq !== state.loadSeq) return;
    state.logs = data;
    const body = $('#logs-table tbody');
    clear(body);
    if (!data.records || data.records.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No log records match.');
      td.colSpan = 5;
      tr.appendChild(td);
      body.appendChild(tr);
    } else {
      for (const record of data.records) {
        const level = normalizeLevel(record.fields?.level);
        const tr = el('tr');
        tr.appendChild(el('td', 'num', fmtTime(record.ts)));
        const levelCell = el('td');
        levelCell.appendChild(el('span', 'level ' + level, level));
        tr.appendChild(levelCell);
        tr.appendChild(el('td', 'mono dim', record.thread_name || threadLabel(record.thread)));
        tr.appendChild(el('td', 'dim', record.fields?.category || '—'));
        tr.appendChild(el('td', 'wrap', record.fields?.msg || ''));
        tr.addEventListener('click', () => openEvent(record.id));
        body.appendChild(tr);
      }
    }
    $('#logs-summary').textContent = `${fmtNum(data.returned || 0)} records in ${fmtTime(state.window.from)} – ${fmtTime(state.window.to)}${data.truncated ? ' (truncated at limit)' : ''}`;
  }

  /* -------------------------------- state ------------------------------- */
  function setTableHead(table, labels, numeric = []) {
    const head = table.querySelector('thead tr');
    if (!head) return;
    clear(head);
    labels.forEach((label, index) => head.appendChild(el('th', numeric.includes(index) ? 'num' : '', label)));
  }

  async function loadState(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const diffMode = $('#state-diff-toggle').checked;
    if (!diffMode) {
      const data = await api('/api/state', { at: state.at });
      if (seq !== state.loadSeq) return;
      state.stateEntries = data;
      setTableHead($('#state-table'), ['key', 'value', 'updated'], [2]);
      const body = $('#state-table tbody');
      clear(body);
      if (!data.entries || data.entries.length === 0) {
        const tr = el('tr');
        const td = el('td', 'empty', 'No snapshot state at this moment.');
        td.colSpan = 3;
        tr.appendChild(td);
        body.appendChild(tr);
      } else {
        for (const entry of data.entries) {
          const tr = el('tr');
          tr.appendChild(el('td', 'mono', entry.key));
          tr.appendChild(el('td', '', entry.type === 'number' ? fmtNum(entry.number) : entry.text));
          tr.appendChild(el('td', 'num', fmtTime(entry.ts)));
          tr.addEventListener('click', () => openStateKey(entry));
          body.appendChild(tr);
        }
      }
      $('#state-summary').textContent = `${data.count || 0} keys at ${fmtTime(state.at)} · last write wins`;
      return;
    }
    if (state.a === null || state.b === null) {
      $('#state-summary').textContent = 'Set A and B with the [ and ] keys (or the timeline buttons) to compare state.';
      setTableHead($('#state-table'), ['key', 'value', 'updated'], [2]);
      clear($('#state-table tbody'));
      const tr = el('tr');
      const td = el('td', 'empty', 'No A/B moments set.');
      td.colSpan = 3;
      tr.appendChild(td);
      $('#state-table tbody').appendChild(tr);
      return;
    }
    const data = await api('/api/diff', { a: state.a, b: state.b });
    if (seq !== state.loadSeq) return;
    state.diff = data;
    setTableHead($('#state-table'), ['key', 'before → after', 'changed'], [2]);
    const body = $('#state-table tbody');
    clear(body);
    if (!data.state || data.state.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'State is identical at A and B.');
      td.colSpan = 3;
      tr.appendChild(td);
      body.appendChild(tr);
    } else {
      for (const entry of data.state) {
        const tr = el('tr');
        tr.appendChild(el('td', 'mono', entry.key));
        tr.appendChild(el('td', entry.changed ? 'pos' : 'dim', `${entry.before || '∅'} → ${entry.after || '∅'}`));
        tr.appendChild(el('td', 'num', entry.changed ? 'yes' : 'no'));
        body.appendChild(tr);
      }
    }
    $('#state-summary').textContent = `state diff ${fmtTime(state.a)} → ${fmtTime(state.b)} · ${data.state.length} changed keys`;
  }

  /* -------------------------------- diff -------------------------------- */
  function ensureAB() {
    if (state.a === null) state.a = 0;
    if (state.b === null) state.b = state.at;
    if (state.b < state.a) { const tmp = state.a; state.a = state.b; state.b = tmp; }
  }

  async function loadDiff(seq = ++state.loadSeq) {
    if (!state.meta) return;
    ensureAB();
    updateABReadout();
    const data = await api('/api/diff', { a: state.a, b: state.b, limit: 150 });
    if (seq !== state.loadSeq) return;
    state.diff = data;
    const cards = $('#diff-cards');
    clear(cards);
    addCard(cards, 'Heap Δ', (data.total_bytes_delta > 0 ? '+' : '') + fmtBytes(data.total_bytes_delta), `${fmtBytes(data.total_bytes_a)} → ${fmtBytes(data.total_bytes_b)}`, data.total_bytes_delta > 0 ? 'warn' : 'good');
    addCard(cards, 'Objects Δ', fmtDelta(data.objects_delta), `${fmtNum(data.objects_a)} → ${fmtNum(data.objects_b)}`);
    addCard(cards, 'Changed sites', fmtNum(data.heap.length), 'largest deltas first');
    addCard(cards, 'Span names Δ', fmtNum(data.spans.length), 'cumulative begins by A vs B');
    addCard(cards, 'State changes', fmtNum(data.state.length), `A ${fmtTime(data.a)} · B ${fmtTime(data.b)}`);
    $('#diff-moments').textContent = `A ${fmtTime(data.a)} · B ${fmtTime(data.b)}`;

    const heapBody = $('#diff-heap tbody');
    clear(heapBody);
    for (const item of data.heap.slice(0, 80)) {
      const tr = el('tr');
      tr.appendChild(el('td', 'mono', item.site));
      tr.appendChild(el('td', 'dim', item.class));
      tr.appendChild(el('td', 'num', fmtBytes(item.bytes_a)));
      tr.appendChild(el('td', 'num', fmtBytes(item.bytes_b)));
      tr.appendChild(el('td', item.bytes_delta > 0 ? 'num pos' : 'num neg', (item.bytes_delta > 0 ? '+' : '') + fmtBytes(item.bytes_delta)));
      tr.appendChild(el('td', 'num', fmtDelta(item.count_delta)));
      tr.addEventListener('click', () => selectSite(item.site, item.class));
      heapBody.appendChild(tr);
    }
    if (data.heap.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No heap change between these moments.');
      td.colSpan = 6;
      tr.appendChild(td);
      heapBody.appendChild(tr);
    }

    const spanBody = $('#diff-spans tbody');
    clear(spanBody);
    for (const item of data.spans.slice(0, 80)) {
      const tr = el('tr');
      tr.appendChild(el('td', 'mono', item.name));
      tr.appendChild(el('td', 'num', fmtDelta(item.count_delta)));
      tr.appendChild(el('td', item.total_ns_delta > 0 ? 'num pos' : 'num neg', (item.total_ns_delta > 0 ? '+' : '') + fmtTime(item.total_ns_delta)));
      tr.addEventListener('click', () => { switchView('spans'); });
      spanBody.appendChild(tr);
    }
    if (data.spans.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No span differences.');
      td.colSpan = 3;
      tr.appendChild(td);
      spanBody.appendChild(tr);
    }

    const counterBody = $('#diff-counters tbody');
    clear(counterBody);
    for (const item of data.counters.slice(0, 60)) {
      const tr = el('tr');
      tr.appendChild(el('td', 'mono', item.name));
      tr.appendChild(el('td', 'num', fmtNum(item.before)));
      tr.appendChild(el('td', 'num', fmtNum(item.after)));
      tr.appendChild(el('td', item.delta > 0 ? 'num pos' : 'num neg', fmtDelta(item.delta)));
      counterBody.appendChild(tr);
    }
    if (data.counters.length === 0) {
      const tr = el('tr');
      const td = el('td', 'empty', 'No counter differences.');
      td.colSpan = 4;
      tr.appendChild(td);
      counterBody.appendChild(tr);
    }

    const stateBox = $('#diff-state');
    clear(stateBox);
    stateBox.appendChild(el('h4', 'muted small', 'State changes'));
    if (data.state.length === 0) emptyMessage(stateBox, 'No state changes.');
    for (const item of data.state.slice(0, 12)) {
      addStackRow(stateBox, item.key, `${item.before || '∅'} → ${item.after || '∅'}`, item.changed ? 'changed' : '');
    }
    const logBox = $('#diff-logs');
    clear(logBox);
    logBox.appendChild(el('h4', 'muted small', 'Log counts by level'));
    if (data.logs.length === 0) emptyMessage(logBox, 'No new logs.');
    for (const item of data.logs) {
      addStackRow(logBox, item.level, `${fmtNum(item.count_a)} → ${fmtNum(item.count_b)}`, fmtDelta(item.delta));
    }
  }

  /* -------------------------------- query ------------------------------- */
  async function runQuery(seq = ++state.loadSeq) {
    if (!state.meta) return;
    const input = $('#query-input');
    const text = input.value.trim();
    const summary = $('#query-summary');
    const body = $('#query-table tbody');
    if (!text) {
      summary.textContent = 'Type a predicate and press Enter.';
      clear(body);
      return;
    }
    try {
      const data = await api('/api/query', { q: text, limit: 500, from: 0, to: state.traceEnd });
      if (seq !== state.loadSeq) return;
      state.query = data;
      summary.textContent = `${fmtNum(data.returned)} records${data.truncated ? ' (truncated)' : ''} · ${data.describe}`;
      clear(body);
      for (const record of data.records) {
        const tr = el('tr');
        tr.appendChild(el('td', 'num', fmtTime(record.ts)));
        tr.appendChild(el('td', 'mono', record.type));
        tr.appendChild(el('td', 'mono dim', record.thread_name || threadLabel(record.thread)));
        tr.appendChild(el('td', 'wrap', fieldSummary(record.fields, 240)));
        tr.addEventListener('click', () => openEvent(record.id));
        body.appendChild(tr);
      }
      if (data.records.length === 0) {
        const tr = el('tr');
        const td = el('td', 'empty', 'No records match.');
        td.colSpan = 4;
        tr.appendChild(td);
        body.appendChild(tr);
      }
    } catch (error) {
      if (error.status === 400) {
        summary.textContent = `query error at ${error.data?.offset ?? 0}: ${error.data?.error || error.message}`;
        clear(body);
        return;
      }
      throw error;
    }
  }

  /* ------------------------------ inspector ----------------------------- */
  function showInspector(title, sections) {
    $('#inspector-title').textContent = title;
    const body = $('#inspector-body');
    clear(body);
    for (const section of sections || []) {
      const box = el('div', 'inspector-section');
      if (section.title) box.appendChild(el('h4', '', section.title));
      if (section.pairs) {
        const dl = el('dl');
        for (const [key, value] of section.pairs) {
          dl.appendChild(el('dt', '', key));
          dl.appendChild(el('dd', '', value === undefined || value === null ? '—' : value));
        }
        box.appendChild(dl);
      }
      if (section.pre !== undefined) box.appendChild(el('pre', '', section.pre));
      if (typeof section.build === 'function') {
        const custom = section.build();
        if (custom) box.appendChild(custom);
      }
      body.appendChild(box);
    }
    $('#inspector').classList.remove('hidden');
  }

  async function openStateKey(entry) {
    try {
      const history = await api('/api/state/history', { key: entry.key, limit: 512 });
      const numeric = history.points.every((point) => point.type === 'number');
      const sections = [
        { title: 'state key', pairs: [
          ['key', entry.key], ['at', fmtTime(state.at)], ['updates', history.count],
          ['current', entry.type === 'number' ? fmtNum(entry.number) : entry.text],
        ] },
      ];
      if (numeric && history.points.length > 1) {
        sections.push({ title: 'history', build: () => {
          const wrap = el('div');
          const canvas = el('canvas');
          canvas.style.width = '100%';
          canvas.style.height = '90px';
          wrap.appendChild(canvas);
          requestAnimationFrame(() => drawSparkline(canvas,
            history.points.map((point) => [point.ts, point.number]), '#5eead4'));
          wrap.appendChild(el('div', 'muted small', `${history.count} snapshots · ${fmtTime(history.points[0].ts)} → ${fmtTime(history.points[history.points.length - 1].ts)}`));
          return wrap;
        } });
      }
      sections.push({ title: 'updates', pre: history.points.map((point) =>
        `${fmtTime(point.ts)}  ${point.type === 'number' ? fmtNum(point.number) : point.text}`).join('\n') || '(none)' });
      showInspector(`state · ${entry.key}`, sections);
    } catch (error) { fail(error); }
  }

  async function openEvent(id) {
    try {
      const record = await api('/api/event', { id });
      const pairs = [
        ['id', record.id],
        ['time', fmtTime(record.ts)],
        ['wall', wallClock(record.ts)],
        ['type', record.type],
        ['thread', `${record.thread_name || threadLabel(record.thread)} (${record.thread})`],
        ['span', record.span || '—'],
      ];
      const fieldPairs = Object.entries(record.fields || {}).map(([key, value]) => {
        if (key === 'frames' && typeof value === 'string') return [key, value.split('\x1f').join(' › ')];
        return [key, typeof value === 'object' ? JSON.stringify(value) : String(value)];
      });
      showInspector(`event #${record.id} · ${record.type}`, [
        { title: 'record', pairs },
        { title: 'fields', pairs: fieldPairs },
        { title: 'raw', pre: JSON.stringify(record, null, 2) },
      ]);
      state.selectedObject = null;
    } catch (error) { fail(error); }
  }

  async function openSpan(id) {
    try {
      const span = await api('/api/span', { id });
      showInspector(`span #${span.id} · ${span.name}`, [
        { title: 'span', pairs: [
          ['id', span.id], ['parent', span.parent || 'root'], ['thread', threadLabel(span.thread)],
          ['depth', span.depth], ['kind', span.kind || '—'],
          ['begin', fmtTime(span.begin_ts)], ['duration', fmtTime(span.duration_ns)],
          ['closed', String(span.closed)], ['children', span.children.join(', ') || 'none'],
        ] },
      ]);
    } catch (error) { fail(error); }
  }

  async function openObject(object) {
    state.selectedObject = object;
    showInspector(`object ${hexOf(object.ptr)} · ${object.class || 'heap'}`, [
      { title: 'allocation', pairs: [
        ['address', hexOf(object.ptr)], ['size', fmtBytes(object.size)], ['class', object.class || '—'],
        ['site', object.site || '—'], ['owner', object.owner === '0x0' ? 'root' : hexOf(object.owner)],
        ['allocated', fmtTime(object.alloc_ts)], ['age at T', fmtTime(object.age_ns)],
      ] },
    ]);
    if (Number.isFinite(Number(object.event_id))) {
      try {
        const record = await api('/api/event', { id: object.event_id });
        const section = el('div', 'inspector-section');
        section.appendChild(el('h4', '', 'allocation record'));
        section.appendChild(el('pre', '', JSON.stringify(record, null, 2)));
        $('#inspector-body').appendChild(section);
      } catch { /* the allocation record may be outside limits */ }
    }
  }

  function openGraphNode(node) {
    showInspector(`retained ${fmtBytes(node.retained)} · ${node.class || 'node'}`, [
      { title: 'object', pairs: [
        ['address', node.ptr], ['size', fmtBytes(node.size)], ['retained', fmtBytes(node.retained)],
        ['owner', node.owner === '0x0' ? 'root' : node.owner], ['site', node.site || '—'],
        ['dominator', node.idom >= 0 ? '#' + node.idom : 'root'], ['children', (node.children || []).length],
      ] },
    ]);
  }

  /* ------------------------------- modals ------------------------------- */
  function openModal(title, build, footer) {
    $('#modal-title').textContent = title;
    const body = $('#modal-body');
    clear(body);
    build(body);
    const foot = $('#modal-foot');
    clear(foot);
    for (const item of footer || []) {
      const button = el('button', item.primary ? 'primary' : '', item.label);
      button.addEventListener('click', () => Promise.resolve(item.action()).catch(fail));
      foot.appendChild(button);
    }
    $('#modal').classList.remove('hidden');
  }

  function closeModal() {
    $('#modal').classList.add('hidden');
  }

  async function openTracePath(path, name) {
    await api('/api/open', {}, { method: 'POST', body: JSON.stringify({ path }) });
    closeModal();
    await reloadAll();
    toast('opened ' + (name || 'trace'));
  }

  function openTracePicker() {
    openModal('Open trace', (body) => {
      const list = el('div', 'trace-choice');
      if (!state.traces.length) {
        list.appendChild(el('div', 'empty', 'No .gbt traces in the configured directories. Record the demo or pass --trace.'));
      }
      for (const trace of state.traces) {
        const button = el('button');
        button.appendChild(el('span', 'name', trace.name));
        button.appendChild(el('span', 'meta', `${fmtBytes(trace.bytes)} · ${new Date(trace.mtime * 1000).toLocaleString()} · ${trace.path}`));
        button.addEventListener('click', guarded(() => openTracePath(trace.path, trace.name)));
        list.appendChild(button);
      }
      body.appendChild(list);
      const form = el('div', 'form-grid');
      form.appendChild(el('label', '', 'Absolute path'));
      const input = el('input');
      input.type = 'text';
      input.placeholder = '/path/to/trace.gbt';
      form.appendChild(input);
      body.appendChild(form);
      input.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') guarded(() => openTracePath(input.value.trim()))();
      });
      const foot = $('#modal-foot');
      clear(foot);
      const cancel = el('button', '', 'Cancel');
      cancel.addEventListener('click', closeModal);
      const open = el('button', 'primary', 'Open path');
      open.addEventListener('click', guarded(() => {
        if (!input.value.trim()) { toast('enter a trace path', true); return; }
        return openTracePath(input.value.trim());
      }));
      append(foot, cancel, open);
    }, []);
  }

  function openDemoDialog() {
    openModal('Record demo workload', (body) => {
      body.appendChild(el('p', 'muted small', 'Records a synthetic game loop with a leak, a latency spike, drifting state, real allocations and stack samples. The UI updates live.'));
      const grid = el('div', 'form-grid');
      const framesLabel = el('label', '', 'Frames');
      const frames = el('input');
      frames.type = 'number';
      frames.min = '10';
      frames.max = '600';
      frames.value = '90';
      const samplerLabel = el('label', '', 'Mach sampler');
      const sampler = el('input');
      sampler.type = 'checkbox';
      const sleepLabel = el('label', '', 'Real 120 ms spike');
      const sleep = el('input');
      sleep.type = 'checkbox';
      sleep.checked = true;
      append(grid, framesLabel, frames, samplerLabel, sampler, sleepLabel, sleep);
      body.appendChild(grid);
      const foot = $('#modal-foot');
      clear(foot);
      const cancel = el('button', '', 'Cancel');
      cancel.addEventListener('click', closeModal);
      const start = el('button', 'primary', 'Record');
      start.addEventListener('click', guarded(async () => {
        const payload = {
          frames: Number(frames.value) || 90,
          sampler: sampler.checked,
          sleep_spike: sleep.checked,
        };
        await api('/api/demo', {}, { method: 'POST', body: JSON.stringify(payload) });
        closeModal();
        startLive();
        toast('recording demo…');
      }));
      append(foot, cancel, start);
    }, []);
  }

  function showWelcome() {
    openModal('Welcome to Glassbox', (body) => {
      body.appendChild(el('p', '', 'Glassbox records CPU samples, allocations, spans, logs, counters and state snapshots, then lets you scrub through them. Everything stays on this machine.'));
      body.appendChild(el('p', 'muted small', 'No trace is open. Record the built-in demo to see a leak, a latency spike and a state drift, or open an existing .gbt file.'));
      const grid = el('div', 'form-grid');
      const record = el('button', 'primary', 'Record the demo');
      record.addEventListener('click', () => { closeModal(); openDemoDialog(); });
      const open = el('button', '', 'Open a trace');
      open.addEventListener('click', () => { closeModal(); openTracePicker(); });
      append(grid, record, open);
      body.appendChild(grid);
    }, []);
  }

  /* ------------------------------- live --------------------------------- */
  function closeLive() {
    if (state.liveSource) {
      try { state.liveSource.close(); } catch { /* ignore */ }
      state.liveSource = null;
    }
    $('#live-badge').classList.add('hidden');
    $('#btn-record').disabled = false;
  }

  function startLive() {
    closeLive();
    state.liveCount = 0;
    $('#live-badge').textContent = 'RECORDING';
    $('#live-badge').classList.remove('hidden');
    $('#btn-record').disabled = true;
    const source = new EventSource('/api/live?t=' + encodeURIComponent(state.token));
    state.liveSource = source;
    source.addEventListener('record', () => {
      state.liveCount++;
      if (state.liveCount % 50 === 0) {
        $('#live-badge').textContent = `RECORDING ${fmtNum(state.liveCount)}`;
      }
    });
    source.addEventListener('gap', (event) => {
      try {
        const data = JSON.parse(event.data);
        toast(`live stream skipped ${data.dropped} events`, true);
      } catch { /* ignore */ }
    });
    source.addEventListener('done', guarded(async (event) => {
      closeLive();
      let info = {};
      try { info = JSON.parse(event.data); } catch { /* ignore */ }
      if (info.ok) {
        toast(`recorded ${fmtNum(info.records)} records · ${fmtNum(info.samples)} samples`);
      } else {
        toast('recording failed: ' + (info.error || 'unknown error'), true);
      }
      await reloadAll();
      setStatus(`${state.meta?.name || 'trace'} · ${state.view} · ${fmtTime(state.at)}`);
    }));
    source.onerror = () => {
      // EventSource retries automatically; only surface a persistent failure.
      if (state.liveSource && state.liveSource.readyState === EventSource.CLOSED) {
        closeLive();
        toast('live stream closed', true);
      }
    };
  }

  /* ------------------------------ loaders ------------------------------- */
  async function loadMeta() {
    const meta = await api('/api/meta');
    state.meta = meta;
    const end = Math.max(1, Number(meta.duration_ns) || 1);
    state.traceEnd = end;
    state.window = { from: 0, to: end };
    state.at = end;
    state.a = null;
    state.b = null;
    state.selectedSite = '';
    state.heapGraph = null;
    state.heapGraphMode = false;
    state.flame = null;
    state.flameZoom = [];
    $('#trace-name').textContent = meta.name;
    $('#trace-path').textContent = meta.path;
    $('#status-records').textContent = `${fmtNum(meta.records)} records · ${fmtNum(meta.chunks)} chunks`;
    $('#version').textContent = 'v' + (meta.glassbox_version || '');
    const stats = $('#session-stats');
    clear(stats);
    const rows = [
      ['records', fmtNum(meta.records)],
      ['duration', fmtTime(meta.duration_ns)],
      ['threads', (meta.threads || []).length],
      ['strings', fmtNum(meta.strings)],
      ['chunks', fmtNum(meta.chunks)],
      ['trace memory', fmtBytes(meta.memory_bytes)],
      ['truncated', meta.truncated ? 'YES' : 'no'],
    ];
    if (meta.warning) rows.push(['warning', meta.warning]);
    for (const [key, value] of rows) {
      stats.appendChild(el('dt', '', key));
      stats.appendChild(el('dd', '', value));
    }
    const linkedAt = resolveMoment(state.linkAt);
    if (linkedAt !== null) state.at = clamp(linkedAt, 0, state.traceEnd);
    const linkedA = resolveMoment(state.linkA);
    const linkedB = resolveMoment(state.linkB);
    if (linkedA !== null) state.a = linkedA;
    if (linkedB !== null) state.b = linkedB;
    state.linkAt = null;
    state.linkA = null;
    state.linkB = null;
    $('#clock').textContent = fmtClock(state.at);
    $('#clock-wall').textContent = wallClock(state.at);
    $('#heap-at').value = state.traceEnd > 0 ? String((state.at / state.traceEnd) * 100) : '100';
    $('#heap-at-label').textContent = fmtTime(state.at);
    updateABReadout();
  }

  async function loadTraceList() {
    try {
      const data = await api('/api/traces');
      state.traces = data.traces || [];
      const list = $('#trace-list');
      clear(list);
      if (state.traces.length === 0) {
        const li = el('li', 'muted small', 'No traces yet');
        list.appendChild(li);
        return;
      }
      for (const trace of state.traces) {
        const li = el('li');
        const button = el('button', data.current === trace.path ? 'current' : '');
        button.appendChild(el('span', 'name', trace.name));
        button.appendChild(el('span', 'meta', `${fmtBytes(trace.bytes)} · ${new Date(trace.mtime * 1000).toLocaleTimeString()}`));
        button.title = trace.path;
        button.addEventListener('click', guarded(async () => {
          await api('/api/open', {}, { method: 'POST', body: JSON.stringify({ path: trace.path }) });
          await reloadAll();
          toast('opened ' + trace.name);
        }));
        li.appendChild(button);
        list.appendChild(li);
      }
    } catch (error) {
      if (error.status !== 404) fail(error);
    }
  }

  async function reloadAll() {
    await loadTraceList();
    try {
      await loadMeta();
    } catch (error) {
      if (error.status === 404) {
        state.meta = null;
        setStatus('No trace open — record the demo or open a trace.');
        return;
      }
      throw error;
    }
    await loadTimeline();
    const initial = location.hash.replace('#', '');
    await switchView(VIEW_NAMES.has(initial) ? initial : 'overview', true, false);
    setStatus(`${state.meta.name} · ${state.view} · ${fmtTime(state.at)}`);
  }

  /* -------------------------------- diff UI ----------------------------- */
  function updateABReadout() {
    const parts = [];
    if (state.a !== null) parts.push('A ' + fmtTime(state.a));
    if (state.b !== null) parts.push('B ' + fmtTime(state.b));
    $('#ab-readout').textContent = parts.join(' · ');
  }

  /* ------------------------------- wiring ------------------------------- */
  function scheduleRender() {
    requestAnimationFrame(() => {
      if (state.view === 'flame') renderFlame();
      else if (state.view === 'heap') renderHeapTreemap();
      else if (state.view === 'spans') drawSpansCanvas();
      drawTimeline();
    });
  }

  function bindTimeline() {
    const canvas = $('#timeline');
    if (!canvas) return;
    const xy = (event) => {
      const rect = canvas.getBoundingClientRect();
      return { x: event.clientX - rect.left, y: event.clientY - rect.top };
    };
    canvas.addEventListener('pointerdown', (event) => {
      const { x } = xy(event);
      state.drag = { x0: x, x1: x, shift: event.shiftKey, moved: false };
      canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (!state.drag) return;
      const { x } = xy(event);
      state.drag.x1 = x;
      if (Math.abs(x - state.drag.x0) > 2) state.drag.moved = true;
      if (!state.drag.shift) {
        setAt(timelineTime(null, x), { refresh: false });
      } else {
        drawTimeline();
      }
    });
    const finish = (event) => {
      if (!state.drag) return;
      const drag = state.drag;
      state.drag = null;
      if (drag.shift && Math.abs((drag.x1 || drag.x0) - drag.x0) > 4) {
        const a = timelineTime(null, drag.x0);
        const b = timelineTime(null, drag.x1 || drag.x0);
        applyWindow(a, b);
        state.a = Math.min(a, b);
        state.b = Math.max(a, b);
        updateABReadout();
      } else if (!drag.moved) {
        setAt(timelineTime(null, drag.x0));
      }
      drawTimeline();
    };
    canvas.addEventListener('pointerup', finish);
    canvas.addEventListener('pointercancel', () => { state.drag = null; drawTimeline(); });
    canvas.addEventListener('pointerleave', () => { if (state.drag) drawTimeline(); });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      const { x } = xy(event);
      const ts = timelineTime(null, x);
      zoomAt(ts, event.deltaY > 0 ? 1.35 : 1 / 1.35);
    }, { passive: false });
  }

  function bindControls() {
    $$('#nav button').forEach((button) => button.addEventListener('click', () => switchView(button.dataset.view)));
    $$('[data-goto]').forEach((link) => link.addEventListener('click', (event) => {
      event.preventDefault();
      switchView(link.dataset.goto);
    }));
    $('#btn-open').addEventListener('click', openTracePicker);
    $('#btn-record').addEventListener('click', openDemoDialog);
    $('#btn-help').addEventListener('click', () => $('#help').classList.remove('hidden'));
    $('#help-close').addEventListener('click', () => $('#help').classList.add('hidden'));
    $('#help').addEventListener('click', (event) => { if (event.target === $('#help')) $('#help').classList.add('hidden'); });
    $('#modal-close').addEventListener('click', closeModal);
    $('#modal').addEventListener('click', (event) => { if (event.target === $('#modal')) closeModal(); });
    $('#inspector-close').addEventListener('click', () => $('#inspector').classList.add('hidden'));

    $('#btn-play').addEventListener('click', togglePlay);
    $('#btn-back').addEventListener('click', () => step(-1));
    $('#btn-forward').addEventListener('click', () => step(1));
    $('#btn-zoom-in').addEventListener('click', () => zoomAt(state.at, 1 / 1.5));
    $('#btn-zoom-out').addEventListener('click', () => zoomAt(state.at, 1.5));
    $('#btn-window-reset').addEventListener('click', resetWindow);
    $('#btn-set-a').addEventListener('click', () => { state.a = state.at; updateABReadout(); toast('A = ' + fmtTime(state.a)); });
    $('#btn-set-b').addEventListener('click', () => { state.b = state.at; updateABReadout(); toast('B = ' + fmtTime(state.b)); });

    $('#btn-jump-heap').addEventListener('click', () => switchView('heap'));
    $('#btn-jump-leaks').addEventListener('click', () => { switchView('heap'); state.selectedSite = ''; setTimeout(() => toast('Leak candidates are on the overview card and in the heap view.'), 50); });
    $('#btn-retained').addEventListener('click', guarded(async () => {
      state.heapGraphMode = !state.heapGraphMode;
      $('#btn-retained').textContent = state.heapGraphMode ? 'Retained: on' : 'Retained graph';
      await loadHeap();
    }));
    $('#heap-at').addEventListener('input', (event) => {
      const ratio = Number(event.target.value) / 100;
      setAt(Math.round(ratio * state.traceEnd), { refresh: true });
    });
    $('#heap-at').addEventListener('change', () => { updateABReadout(); });

    $$('[data-flame-mode]').forEach((button) => button.addEventListener('click', () => {
      state.flameMode = button.dataset.flameMode;
      $$('[data-flame-mode]').forEach((other) => other.classList.toggle('active', other === button));
      loadFlame();
    }));
    $('#flame-search').addEventListener('input', (event) => {
      state.flameSearch = event.target.value;
      if (state.view === 'flame') renderFlame();
    });
    $('#flame-canvas').addEventListener('mousemove', flameHover);
    $('#flame-canvas').addEventListener('mouseleave', () => $('#flame-tooltip').classList.add('hidden'));
    $('#flame-canvas').addEventListener('click', flameClick);

    $('#heap-treemap').addEventListener('mousemove', heapTreemapHover);
    $('#heap-treemap').addEventListener('mouseleave', () => $('#heap-tooltip').classList.add('hidden'));
    $('#heap-treemap').addEventListener('click', heapTreemapClick);

    $('#spans-active-only').addEventListener('change', () => { if (state.view === 'spans') renderSpans(); });

    $$('#log-levels button').forEach((button) => button.addEventListener('click', () => {
      state.logLevel = Number(button.dataset.level);
      $$('#log-levels button').forEach((other) => other.classList.toggle('active', other === button));
      loadLogs();
    }));
    let logTimer = 0;
    $('#log-search').addEventListener('input', (event) => {
      state.logQuery = event.target.value;
      window.clearTimeout(logTimer);
      logTimer = window.setTimeout(() => loadLogs(), 250);
    });

    $('#state-diff-toggle').addEventListener('change', () => loadState());
    $('#btn-diff-swap').addEventListener('click', () => {
      const tmp = state.a;
      state.a = state.b;
      state.b = tmp;
      updateABReadout();
      if (state.view === 'diff') loadDiff(); else loadState();
    });

    $('#query-form').addEventListener('submit', (event) => {
      event.preventDefault();
      runQuery();
    });

    window.addEventListener('resize', scheduleRender);
    window.addEventListener('hashchange', () => {
      const name = location.hash.replace('#', '');
      if (VIEW_NAMES.has(name) && name !== state.view) switchView(name, true, false);
    });
    document.addEventListener('keydown', (event) => {
      if (event.target instanceof HTMLInputElement || event.target instanceof HTMLTextAreaElement) {
        if (event.key === 'Escape') event.target.blur();
        return;
      }
      if (event.key === 'Escape') {
        $('#help').classList.add('hidden');
        $('#inspector').classList.add('hidden');
        closeModal();
        return;
      }
      if (event.key === ' ') { event.preventDefault(); togglePlay(); return; }
      if (event.key === 'ArrowLeft') { event.preventDefault(); step(event.shiftKey ? -10 : -1); return; }
      if (event.key === 'ArrowRight') { event.preventDefault(); step(event.shiftKey ? 10 : 1); return; }
      if (event.key === '[') { state.a = state.at; updateABReadout(); return; }
      if (event.key === ']') { state.b = state.at; updateABReadout(); return; }
      if (event.key === '/') { event.preventDefault(); switchView('query'); return; }
      if (event.key === 'g') { state.waitingForG = true; window.setTimeout(() => { state.waitingForG = false; }, 900); return; }
      if (state.waitingForG) {
        const map = { o: 'overview', f: 'flame', h: 'heap', s: 'spans', l: 'logs', v: 'state', d: 'diff' };
        if (map[event.key]) { switchView(map[event.key]); state.waitingForG = false; }
      }
    });
  }

  function step(direction) {
    const range = Math.max(1, state.window.to - state.window.from);
    const delta = Math.max(1, Math.round(range / 120)) * direction;
    setAt(state.at + delta);
  }

  function togglePlay() {
    state.playing = !state.playing;
    $('#btn-play').textContent = state.playing ? '❚❚' : '▶';
    if (state.playing) {
      const tick = () => {
        if (!state.playing) return;
        const range = Math.max(1, state.window.to - state.window.from);
        const delta = Math.max(1, range / 240);
        let next = state.at + delta;
        if (next >= state.window.to) { next = state.window.from; }
        setAt(next, { refresh: true });
        state.playTimer = window.setTimeout(tick, 60);
      };
      tick();
    } else {
      window.clearTimeout(state.playTimer);
    }
  }

  /* -------------------------------- boot -------------------------------- */
  async function boot() {
    initToken();
    bindControls();
    bindTimeline();
    if (!state.token) {
      setStatus('No session token. Relaunch the Glassbox launcher.');
      toast('Missing session token: relaunch the launcher.', true);
      return;
    }
    try {
      const health = await api('/api/health');
      $('#version').textContent = 'v' + health.version;
      if (health.recording) startLive();
    } catch (error) {
      if (error.status === 403) {
        setStatus('Session token rejected. Relaunch the Glassbox launcher.');
        toast('Session token rejected — relaunch the launcher.', true);
        return;
      }
      fail(error);
    }
    await loadTraceList();
    try {
      await loadMeta();
    } catch (error) {
      if (error.status === 404) {
        $('#trace-name').textContent = 'No trace open';
        $('#trace-path').textContent = '';
        setStatus('No trace open — record the demo or open a trace.');
        showWelcome();
        return;
      }
      fail(error);
      return;
    }
    await loadTimeline();
    const initial = location.hash.replace('#', '');
    await switchView(VIEW_NAMES.has(initial) ? initial : 'overview', true, false);
    setStatus(`${state.meta.name} · ${state.view} · ${fmtTime(state.at)}`);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', guarded(boot));
  } else {
    guarded(boot)();
  }
})();
