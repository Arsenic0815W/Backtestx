/* BacktestX report renderer. Vanilla JS, no network access, no libraries.
 *
 * THIS FILE COMPUTES NO FINANCIAL NUMBER. Every metric, return, drawdown, fee total and rank in the report is computed by the C++
 * report builder (src/report.cpp) from the exported run files and embedded as JSON; this code only formats and draws it.
 * (Axis scaling, sorting and table look-ups are display operations.)
 * Everything from the data is inserted with textContent / setAttribute, never as an HTML string.
 */
(function (root) {
  'use strict';

  /* ------------------------------------------------------------------------------------------ formatting (pure) */
  var RUPEE = '₹';

  // Indian digit grouping of a string of digits: 12345678 -> 1,23,45,678
  function groupIndian(digits) {
    if (digits.length <= 3) return digits;
    var last3 = digits.slice(-3), rest = digits.slice(0, -3), parts = [];
    while (rest.length > 2) { parts.unshift(rest.slice(-2)); rest = rest.slice(0, -2); }
    if (rest.length) parts.unshift(rest);
    return parts.join(',') + ',' + last3;
  }

  // Integer paise -> "₹1,23,456.78" (integer arithmetic only; the minus sign goes in front of the symbol).
  function inr(paise, withSymbol) {
    if (paise === null || paise === undefined || !isFinite(paise)) return 'n/a';
    var neg = paise < 0, a = Math.abs(Math.round(paise));
    var p = a % 100, rupees = (a - p) / 100;
    var s = groupIndian(String(rupees)) + '.' + (p < 10 ? '0' : '') + p;
    return (neg ? '-' : '') + (withSymbol === false ? '' : RUPEE) + s;
  }

  // Rupee amount for an axis label: 12,345 / 1.5L / 2.4Cr (lakh = 1e5, crore = 1e7).
  function inrAxis(rupees) {
    var a = Math.abs(rupees), sgn = rupees < 0 ? '-' : '';
    function trim(x) { return (Math.round(x * 100) / 100).toString(); }
    if (a >= 1e7) return sgn + RUPEE + trim(a / 1e7) + 'Cr';
    if (a >= 1e5) return sgn + RUPEE + trim(a / 1e5) + 'L';
    return sgn + RUPEE + groupIndian(String(Math.round(a)));
  }

  function pct(x, d) { return (x === null || x === undefined) ? 'n/a' : (x * 100).toFixed(d === undefined ? 2 : d) + '%'; }
  function spct(x, d) {
    if (x === null || x === undefined) return 'n/a';
    var t = (x * 100).toFixed(d === undefined ? 2 : d);
    return (x > 0 && t.charAt(0) !== '-' ? '+' : '') + t + '%';
  }
  function num(x, d) { return (x === null || x === undefined) ? 'n/a' : x.toFixed(d === undefined ? 2 : d); }
  function int(x) { return (x === null || x === undefined) ? 'n/a' : groupIndian(String(Math.abs(x))).replace(/^/, x < 0 ? '-' : ''); }

  var MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  function dayNum(iso) { return Date.UTC(+iso.slice(0, 4), +iso.slice(5, 7) - 1, +iso.slice(8, 10)) / 86400000; }
  function dayIso(n) { return new Date(n * 86400000).toISOString().slice(0, 10); }

  function niceTicks(lo, hi, n) {
    if (!(hi > lo)) return [lo];
    var raw = (hi - lo) / Math.max(1, n), mag = Math.pow(10, Math.floor(Math.log10(raw))), norm = raw / mag;
    var step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag, out = [];
    for (var v = Math.ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-9; v += step) out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
    return out;
  }
  function logTicks(lo, hi) {
    var out = [], e0 = Math.floor(Math.log10(lo)), e1 = Math.ceil(Math.log10(hi)), mults = [1, 2, 5];
    for (var e = e0; e <= e1; e++)
      for (var i = 0; i < mults.length; i++) { var v = mults[i] * Math.pow(10, e); if (v >= lo && v <= hi) out.push(v); }
    return out.length >= 3 ? out : niceTicks(lo, hi, 4);
  }
  // Calendar ticks: month starts, thinned so that labels do not collide.
  function timeTicks(d0, d1, width) {
    var a = new Date(d0 * 86400000), b = new Date(d1 * 86400000), starts = [];
    var y = a.getUTCFullYear(), m = a.getUTCMonth();
    for (;;) {
      var t = Date.UTC(y, m, 1) / 86400000;
      if (t > d1) break;
      if (t >= d0) starts.push({ day: t, y: y, m: m });
      m++; if (m > 11) { m = 0; y++; }
    }
    var steps = [1, 2, 3, 6, 12, 24, 60, 120], maxLabels = Math.max(2, Math.floor(width / 78));
    for (var s = 0; s < steps.length; s++) {
      var pick = starts.filter(function (t) { return steps[s] >= 12 ? (t.m === 0 && t.y % (steps[s] / 12) === 0) : (t.m % steps[s] === 0); });
      if (pick.length <= maxLabels || s === steps.length - 1)
        return pick.map(function (t) { return { day: t.day, label: steps[s] >= 12 ? String(t.y) : (t.m === 0 ? String(t.y) : MONTHS[t.m]) + (t.m === 0 ? '' : " '" + String(t.y).slice(2)) }; });
    }
    return [];
  }

  // Colour scales (display only). Dark surface; diverging pair blue(+) <-> red(-) through a neutral grey; sequential blue ramp.
  var NEUTRAL = [56, 56, 53], POS = [57, 135, 229], NEG = [230, 103, 103];
  var SEQ_LO = [13, 54, 107], SEQ_HI = [158, 197, 244];
  function mix(a, b, t) { return [Math.round(a[0] + (b[0] - a[0]) * t), Math.round(a[1] + (b[1] - a[1]) * t), Math.round(a[2] + (b[2] - a[2]) * t)]; }
  function rgb(c) { return 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')'; }
  function lum(c) {
    var f = c.map(function (v) { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); });
    return 0.2126 * f[0] + 0.7152 * f[1] + 0.0722 * f[2];
  }
  function textOn(c) { return lum(c) > 0.3 ? '#0b0b0b' : '#f0efec'; }
  function divergingColor(v, maxAbs) {
    var t = maxAbs > 0 ? Math.min(1, Math.abs(v) / maxAbs) : 0;
    var c = mix(NEUTRAL, v >= 0 ? POS : NEG, t * 0.9);
    return { bg: rgb(c), fg: textOn(c) };
  }
  function sequentialColor(v, lo, hi) {
    var t = hi > lo ? (v - lo) / (hi - lo) : 0.5;
    var c = mix(SEQ_LO, SEQ_HI, Math.max(0, Math.min(1, t)));
    return { bg: rgb(c), fg: textOn(c) };
  }

  /* ----------------------------------------------------------------------------------------------- DOM helpers */
  var SVGNS = 'http://www.w3.org/2000/svg';
  function make(ns, tag, attrs, kids) {
    var el = ns ? document.createElementNS(ns, tag) : document.createElement(tag);
    if (attrs) Object.keys(attrs).forEach(function (k) {
      var v = attrs[k];
      if (v === null || v === undefined || v === false) return;
      if (k === 'class') el.setAttribute('class', v);
      else if (k.slice(0, 2) === 'on') el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? '' : String(v));
    });
    (function add(list) {
      list.forEach(function (c) {
        if (c === null || c === undefined || c === false) return;
        if (Array.isArray(c)) return add(c);
        el.appendChild(typeof c === 'object' ? c : document.createTextNode(String(c)));
      });
    })(kids || []);
    return el;
  }
  function h(tag, attrs) { return make(null, tag, attrs, Array.prototype.slice.call(arguments, 2)); }
  function sv(tag, attrs) { return make(SVGNS, tag, attrs, Array.prototype.slice.call(arguments, 2)); }

  var tipEl = null;
  function tip() {
    if (!tipEl) { tipEl = h('div', { id: 'tip', role: 'tooltip' }); document.body.appendChild(tipEl); }
    return tipEl;
  }
  // rows: [{label, value, color}]; title: string
  function showTip(ev, title, rows) {
    var t = tip();
    t.textContent = '';
    t.appendChild(h('div', { class: 't' }, title));
    rows.forEach(function (r) {
      t.appendChild(h('div', { class: 'r' },
        h('span', null, r.color ? h('span', { class: 'sw', style: 'background:' + r.color }) : null, r.label),
        h('span', null, r.value)));
    });
    t.style.display = 'block';
    var x = ev.clientX + 14, y = ev.clientY + 14, w = t.offsetWidth, hgt = t.offsetHeight;
    if (x + w > window.innerWidth - 8) x = Math.max(8, ev.clientX - w - 14);
    if (y + hgt > window.innerHeight - 8) y = Math.max(8, ev.clientY - hgt - 14);
    t.style.left = x + 'px'; t.style.top = y + 'px';
  }
  function hideTip() { if (tipEl) tipEl.style.display = 'none'; }
  function tipOn(el, fn) {
    el.addEventListener('pointermove', function (ev) { var r = fn(ev); if (r) showTip(ev, r.title, r.rows); });
    el.addEventListener('pointerleave', hideTip);
    el.addEventListener('focus', function () {
      var b = el.getBoundingClientRect(), r = fn({ clientX: b.left, clientY: b.bottom });
      if (r) showTip({ clientX: b.left, clientY: b.bottom }, r.title, r.rows);
    });
    el.addEventListener('blur', hideTip);
  }

  /* --------------------------------------------------------------------------------------------------- line chart */
  // spec: {series:[{name,color,x:[day],y:[v|null],area:bool}], log, yFmt(v), tipFmt(v), height, pad:[bottom,top], baseline}
  function lineChart(host, spec) {
    host.textContent = '';
    var W = Math.max(280, host.clientWidth || 800), H = spec.height || (W < 560 ? 240 : 320);
    var m = { l: W < 560 ? 60 : 76, r: W < 560 ? 44 : 84, t: 10, b: 26 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b, log = !!spec.log;
    var x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    spec.series.forEach(function (s) {
      for (var i = 0; i < s.x.length; i++) {
        var y = s.y[i];
        if (y === null || y === undefined || (log && y <= 0)) continue;
        if (s.x[i] < x0) x0 = s.x[i]; if (s.x[i] > x1) x1 = s.x[i];
        if (y < y0) y0 = y; if (y > y1) y1 = y;
      }
    });
    var svg = sv('svg', { viewBox: '0 0 ' + W + ' ' + H, width: W, height: H, role: 'img', 'aria-label': spec.label || 'chart' });
    host.appendChild(svg);
    if (!isFinite(x0)) { svg.appendChild(sv('text', { x: 10, y: 24 }, 'no data to draw')); return; }
    if (spec.include !== undefined) { y0 = Math.min(y0, spec.include); y1 = Math.max(y1, spec.include); }
    var padB = spec.pad ? spec.pad[0] : 0.05, padT = spec.pad ? spec.pad[1] : 0.05;
    var ly0, ly1;
    if (log) { ly0 = Math.log(y0); ly1 = Math.log(y1); var sp = (ly1 - ly0) || 0.1; ly0 -= sp * padB; ly1 += sp * padT; }
    else { var sp2 = (y1 - y0) || Math.abs(y1) * 0.1 || 1; y0 -= sp2 * padB; y1 += sp2 * padT; }
    if (x1 === x0) x1 = x0 + 1;
    function sx(d) { return m.l + (d - x0) / (x1 - x0) * pw; }
    function sy(v) { return log ? m.t + (1 - (Math.log(v) - ly0) / (ly1 - ly0)) * ph : m.t + (1 - (v - y0) / (y1 - y0)) * ph; }

    var yt = log ? logTicks(Math.exp(ly0), Math.exp(ly1)) : niceTicks(y0, y1, Math.max(3, Math.floor(ph / 52)));
    yt.forEach(function (v) {
      var y = sy(v);
      if (y < m.t - 1 || y > m.t + ph + 1) return;
      svg.appendChild(sv('line', { class: 'gl', x1: m.l, x2: m.l + pw, y1: y, y2: y }));
      svg.appendChild(sv('text', { x: m.l - 8, y: y + 4, 'text-anchor': 'end' }, spec.yFmt(v)));
    });
    svg.appendChild(sv('line', { class: 'ax', x1: m.l, x2: m.l + pw, y1: m.t + ph, y2: m.t + ph }));
    timeTicks(x0, x1, pw).forEach(function (t) {
      var x = sx(t.day);
      svg.appendChild(sv('line', { class: 'gl', x1: x, x2: x, y1: m.t, y2: m.t + ph }));
      svg.appendChild(sv('text', { x: x, y: H - 8, 'text-anchor': 'middle' }, t.label));
    });

    var ends = [];
    spec.series.forEach(function (s) {
      var d = '', started = false, last = null, firstX = null, areaD = '';
      for (var i = 0; i < s.x.length; i++) {
        var y = s.y[i];
        if (y === null || y === undefined || (log && y <= 0)) { started = false; continue; }
        var px = sx(s.x[i]).toFixed(1), py = sy(y).toFixed(1);
        d += (started ? 'L' : 'M') + px + ' ' + py; started = true;
        if (firstX === null) firstX = px;
        last = { x: px, y: py, v: y };
      }
      if (s.area && last && spec.baseline !== undefined) {
        var by = sy(spec.baseline).toFixed(1);
        svg.appendChild(sv('path', { class: 'ar', d: d + 'L' + last.x + ' ' + by + 'L' + firstX + ' ' + by + 'Z', fill: s.color }));
      }
      svg.appendChild(sv('path', { class: 'ln', d: d, stroke: s.color, 'data-series': s.name }));
      if (last) ends.push({ name: s.name, color: s.color, x: +last.x, y: +last.y });
    });
    // direct labels at the right edge, pushed apart if they would overlap
    ends.sort(function (a, b) { return a.y - b.y; });
    for (var k = 1; k < ends.length; k++) if (ends[k].y - ends[k - 1].y < 13) ends[k].y = ends[k - 1].y + 13;
    if (W >= 560) ends.forEach(function (e) {
      svg.appendChild(sv('text', { class: 'endlbl', x: Math.min(e.x + 6, W - 2), y: e.y + 4, fill: e.color, style: 'fill:' + e.color }, e.name.length > 12 ? e.name.slice(0, 11) + '…' : e.name));
    });

    // hover layer
    var cross = sv('line', { class: 'cross', y1: m.t, y2: m.t + ph, style: 'display:none' });
    svg.appendChild(cross);
    var dots = spec.series.map(function (s) { var c = sv('circle', { class: 'dot', r: 4, fill: s.color, style: 'display:none' }); svg.appendChild(c); return c; });
    var hit = sv('rect', { x: m.l, y: m.t, width: pw, height: ph, fill: 'transparent' });
    svg.appendChild(hit);
    function nearest(xs, d) {
      var lo = 0, hi = xs.length - 1;
      while (hi - lo > 1) { var mid = (lo + hi) >> 1; if (xs[mid] < d) lo = mid; else hi = mid; }
      return Math.abs(xs[lo] - d) <= Math.abs(xs[hi] - d) ? lo : hi;
    }
    hit.addEventListener('pointermove', function (ev) {
      var r = svg.getBoundingClientRect(), px = (ev.clientX - r.left) * (W / r.width);
      var d = x0 + (px - m.l) / pw * (x1 - x0), rows = [], title = null;
      spec.series.forEach(function (s, si) {
        if (!s.x.length) return;
        var i = nearest(s.x, d), y = s.y[i];
        if (y === null || y === undefined || Math.abs(s.x[i] - d) > 10) { dots[si].style.display = 'none'; return; }
        if (title === null) title = dayIso(s.x[i]);
        rows.push({ label: s.name, value: (spec.tipFmt || spec.yFmt)(y), color: s.color });
        if (!log || y > 0) {
          dots[si].setAttribute('cx', sx(s.x[i])); dots[si].setAttribute('cy', sy(y)); dots[si].style.display = '';
          cross.setAttribute('x1', sx(s.x[i])); cross.setAttribute('x2', sx(s.x[i])); cross.style.display = '';
        }
      });
      if (title !== null) showTip(ev, title, rows);
    });
    hit.addEventListener('pointerleave', function () { hideTip(); cross.style.display = 'none'; dots.forEach(function (c) { c.style.display = 'none'; }); });
  }

  function legend(series) {
    return h('div', { class: 'legend' }, series.map(function (s) { return h('span', null, h('i', { style: 'background:' + s.color }), s.name); }));
  }

  // Re-draw on width changes (debounced by animation frame).
  function onResize(fn) {
    if (typeof ResizeObserver === 'undefined') return;
    var last = -1, pending = false;
    var ro = new ResizeObserver(function (entries) {
      var w = Math.round(entries[0].contentRect.width);
      if (w === last || w === 0) { last = w; return; }
      last = w;
      if (pending) return;
      pending = true;
      requestAnimationFrame(function () { pending = false; fn(); });
    });
    return ro;
  }

  /* ------------------------------------------------------------------------------------------------ report parts */
  var DISCLAIMER = 'Not investment advice. Past performance does not predict future results.';

  function metricRows(S, B) {
    var e = S.equity, b = B ? B.metrics : null;
    function row(label, key, f) { return { label: label, s: f(e[key]), b: b ? f(b[key]) : null }; }
    return [
      row('Total return', 'total_return', pct), row('CAGR', 'cagr', pct), row('Volatility (annualised)', 'annualised_volatility', pct),
      row('Sharpe', 'sharpe', num), row('Sortino', 'sortino', num), row('Max drawdown', 'max_drawdown', pct), row('Calmar', 'calmar', num),
      row('Best day', 'best_day', spct), row('Worst day', 'worst_day', spct)
    ];
  }

  // Every wide table lives in its own horizontally scrolling container, so the PAGE never grows wider than the viewport.
  function kv(rows) {
    return h('div', { class: 'scroll' }, h('table', null, h('tbody', null, rows.map(function (r) {
      return h('tr', null, h('td', { class: 'sec' }, r[0]), h('td', { class: 'n' }, r[1]), r[2] ? h('td', { class: 'muted' }, r[2]) : null);
    }))));
  }

  function tilesSection(D) {
    var e = D.summary.equity, t = D.summary.trades;
    function tile(k, v, d, cls) { return h('div', { class: 'tile' }, h('div', { class: 'k' }, k), h('div', { class: 'v ' + (cls || '') }, v), d ? h('div', { class: 'd' }, d) : null); }
    return h('div', { class: 'tiles' },
      tile('End equity', inr(e.end_equity_paise), 'from ' + inr(e.start_equity_paise)),
      tile('Total return', spct(e.total_return), D.benchmark ? 'benchmark ' + spct(D.benchmark.metrics.total_return) : null, e.total_return > 0 ? 'pos' : e.total_return < 0 ? 'neg' : ''),
      tile('CAGR', spct(e.cagr), e.years === null ? null : num(e.years, 2) + ' years'),
      tile('Sharpe', num(e.sharpe), D.benchmark ? 'benchmark ' + num(D.benchmark.metrics.sharpe) : null),
      tile('Max drawdown', pct(e.max_drawdown), D.benchmark ? 'benchmark ' + pct(D.benchmark.metrics.max_drawdown) : null),
      tile('Closed trades', int(t.num_trades), 'fees ' + inr(t.total_fees_paise)));
  }

  function metricsSection(D) {
    var S = D.summary, B = D.benchmark, e = S.equity, t = S.trades;
    var rows = metricRows(S, B);
    var head = h('tr', null, h('th', null, 'Metric'), h('th', { class: 'n' }, 'Strategy'), B ? h('th', { class: 'n' }, B.name + ' (benchmark)') : null);
    var main = h('table', null, h('thead', null, head), h('tbody', null, rows.map(function (r) {
      return h('tr', null, h('td', { class: 'sec' }, r.label), h('td', { class: 'n' }, r.s), B ? h('td', { class: 'n' }, r.b) : null);
    })));
    var rel = null;
    if (S.benchmark) {
      var b = S.benchmark;
      rel = [h('h3', null, 'Relative to ' + b.name), kv([
        ['Excess return', spct(b.excess_return)], ['Beta', num(b.beta, 3)], ['Alpha (annualised)', spct(b.alpha_annual)],
        ['Correlation of daily returns', num(b.correlation, 3)], ['Paired returns', int(b.n_pairs)],
        ['Common dates', int(b.n_common_dates) + (b.first_common_date ? ' (' + b.first_common_date + ' to ' + b.last_common_date + ')' : '')]])];
    }
    var dd = [['Max drawdown', pct(e.max_drawdown)], ['Peak / trough', (e.max_drawdown_peak_date || 'n/a') + ' / ' + (e.max_drawdown_trough_date || 'n/a')],
      ['Recovered', e.max_drawdown_recovered === null ? 'n/a' : (e.max_drawdown_recovered ? 'yes, ' + (e.max_drawdown_recovery_date || '') : 'not by ' + e.end_date)],
      ['Drawdown duration', e.max_drawdown_duration_days === null ? 'n/a' : int(e.max_drawdown_duration_days) + ' days'],
      ['Longest underwater', e.longest_underwater_days === null ? 'n/a' : int(e.longest_underwater_days) + ' days'],
      ['Time in market', pct(e.time_in_market, 1)], ['Average exposure', pct(e.avg_exposure, 1)]];
    var tr = [['Closed trades', int(t.num_trades)], ['Wins / losses / breakeven', int(t.num_wins) + ' / ' + int(t.num_losses) + ' / ' + int(t.num_breakeven)],
      ['Win rate', pct(t.win_rate, 1)], ['Average win', inr(t.avg_win_paise)], ['Average loss', inr(t.avg_loss_paise)],
      ['Profit factor', num(t.profit_factor)], ['Best trade', inr(t.best_trade_paise)], ['Worst trade', inr(t.worst_trade_paise)],
      ['Average holding', t.avg_holding_days === null ? 'n/a' : num(t.avg_holding_days, 1) + ' days'], ['Fills', int(t.num_fills)],
      ['Open positions at end', int(t.num_open_positions)], ['Traded value', inr(t.traded_value_paise)],
      ['Annual turnover (one-way)', num(t.turnover_annual, 2) + 'x'],
      ['Closed-trade net P&L (after fees)', inr(D.pnl.closed_trades_net_paise)],
      ['Unrealised P&L (open positions, after their entry fees)', inr(D.pnl.unrealised_paise)],
      ['Total P&L (change in equity)', inr(D.pnl.total_paise)]];
    var note = 'Trade statistics use net P&L after fees on closed round trips. Risk-free rate ' + pct(S.config.risk_free_annual, 2) +
      ' a year (an assumption). ' + (B ? 'The benchmark column is the benchmark rebased to the strategy’s equity on the first common date and measured with the same code on the dates both series share.' : 'No benchmark was configured.');
    return h('section', { class: 'card', id: 'metrics' }, h('h2', null, 'Performance' + (B ? ' vs ' + B.name : '')),
      h('div', { class: 'two' },
        h('div', null, h('div', { class: 'scroll' }, main), rel, h('h3', null, 'Drawdown and exposure'), kv(dd)),
        h('div', null, h('h3', { style: 'margin-top:0' }, 'Trades'), kv(tr))),
      h('p', { class: 'note' }, note));
  }

  function equitySection(D) {
    var E = D.equity, B = D.benchmark;
    var xs = E.dates.map(dayNum);
    var series = [{ name: 'Strategy', color: 'var(--series-1)', x: xs, y: E.equity_paise.map(function (p) { return p / 100; }) }];
    var colors = { 'Strategy': '#3987e5' };
    series[0].color = colors.Strategy;
    var minEq = Math.min.apply(null, E.equity_paise);
    if (B) {
      series.push({ name: B.name, color: '#d95926', x: B.dates.map(dayNum), y: B.equity_paise.map(function (p) { return p / 100; }) });
      minEq = Math.min(minEq, Math.min.apply(null, B.equity_paise));
    }
    var canLog = minEq > 0, state = { log: false };
    var chart = h('div', { class: 'chart', id: 'equity-chart' });
    var bLin = h('button', { type: 'button', 'aria-pressed': 'true' }, 'Linear');
    var bLog = h('button', { type: 'button', 'aria-pressed': 'false', disabled: !canLog, title: canLog ? 'Logarithmic y axis' : 'Disabled: equity is zero or negative at some point' }, 'Log');
    function draw() {
      bLin.setAttribute('aria-pressed', String(!state.log)); bLog.setAttribute('aria-pressed', String(state.log));
      lineChart(chart, { series: series, log: state.log, yFmt: inrAxis, tipFmt: function (v) { return inr(Math.round(v * 100)); }, label: 'Equity curve' });
    }
    bLin.addEventListener('click', function () { state.log = false; draw(); });
    bLog.addEventListener('click', function () { if (canLog) { state.log = true; draw(); } });
    var ro = onResize(draw);
    var sec = h('section', { class: 'card', id: 'equity' },
      h('div', { class: 'row' }, h('h2', { class: 'grow', style: 'margin:0' }, 'Equity curve' + (B ? ' vs ' + B.name : '')), h('div', { class: 'seg' }, bLin, bLog)),
      legend(series), chart,
      h('p', { class: 'note' }, B ? 'The benchmark is rescaled to start at the strategy’s equity on the first date both series have a value; it is drawn only on those dates.' : 'No benchmark configured.'));
    return { el: sec, after: function () { draw(); if (ro) ro.observe(chart); } };
  }

  function drawdownSection(D) {
    var E = D.equity, B = D.benchmark;
    var series = [{ name: 'Strategy', color: '#3987e5', x: E.dates.map(dayNum), y: E.drawdown, area: true }];
    if (B) series.push({ name: B.name, color: '#d95926', x: B.dates.map(dayNum), y: B.drawdown });
    var chart = h('div', { class: 'chart', id: 'drawdown-chart' });
    function draw() { lineChart(chart, { series: series, yFmt: function (v) { return pct(v, 0); }, tipFmt: function (v) { return pct(v, 2); }, baseline: 0, include: 0, pad: [0.06, 0], label: 'Drawdown' }); }
    var ro = onResize(draw);
    var sec = h('section', { class: 'card', id: 'drawdown' }, h('h2', null, 'Drawdown (distance below the previous peak)'), legend(series), chart);
    return { el: sec, after: function () { draw(); if (ro) ro.observe(chart); } };
  }

  function monthlySection(D) {
    var mon = D.monthly, yrs = D.yearly;
    var byYear = {}, years = [], maxAbs = 0;
    mon.forEach(function (r) {
      if (!byYear[r.y]) { byYear[r.y] = {}; years.push(r.y); }
      byYear[r.y][r.m] = r;
      if (r.r !== null) maxAbs = Math.max(maxAbs, Math.abs(r.r));
    });
    var yearTot = {};
    yrs.forEach(function (y) { yearTot[y.y] = y; if (y.r !== null) maxAbs = Math.max(maxAbs, Math.abs(y.r)); });
    var head = h('tr', null, h('th', null, ''), MONTHS.map(function (n) { return h('th', null, n); }), h('th', null, 'Year'));
    var body = years.map(function (y) {
      var cells = [];
      for (var mth = 1; mth <= 12; mth++) {
        var r = byYear[y][mth];
        if (!r || r.r === null) { cells.push(h('td', { class: 'empty' }, r ? 'n/a' : '')); continue; }
        var c = divergingColor(r.r, maxAbs), partial = r.first || r.last;
        var td = h('td', { class: partial ? 'partial' : '', style: 'background:' + c.bg + ';color:' + c.fg, tabindex: 0 }, spct(r.r, 1));
        tipOn(td, (function (r, y, mth) { return function () {
          return { title: MONTHS[mth - 1] + ' ' + y, rows: [{ label: 'Return', value: spct(r.r, 2) }, { label: 'From', value: r.from }, { label: 'To', value: r.to }].concat(partial ? [{ label: 'Note', value: r.first ? 'starts at the first record' : 'ends at the last record' }] : []) };
        }; })(r, y, mth));
        cells.push(td);
      }
      var yt = yearTot[y], ytd = null;
      if (yt && yt.r !== null) {
        var c2 = divergingColor(yt.r, maxAbs);
        ytd = h('td', { class: 'tot', style: 'background:' + c2.bg + ';color:' + c2.fg }, spct(yt.r, 1));
      } else ytd = h('td', { class: 'empty' }, 'n/a');
      return h('tr', null, h('th', { class: 'yr' }, String(y)), cells, ytd);
    });
    var ramp = h('div', { class: 'ramp' }, spct(-maxAbs, 1), h('i', { style: 'background:linear-gradient(90deg,' + rgb(mix(NEUTRAL, NEG, 0.9)) + ',' + rgb(NEUTRAL) + ',' + rgb(mix(NEUTRAL, POS, 0.9)) + ')' }), spct(maxAbs, 1));
    return h('section', { class: 'card', id: 'monthly' }, h('h2', null, 'Monthly returns'),
      mon.length ? h('div', { class: 'scroll' }, h('table', { class: 'heat' }, h('thead', null, head), h('tbody', null, body))) : h('p', { class: 'muted' }, 'The curve is too short to form a monthly return.'),
      mon.length ? ramp : null,
      h('p', { class: 'note' }, 'Return of a month = last equity record of the month / last record of the previous month - 1; the first month starts from the first record. Dashed outline: the first and last month, which may cover only part of a calendar month. Year = compounded.'));
  }

  function tradesSection(D) {
    var T = D.trades, cols = T.columns, rows = T.rows.slice();
    var ix = {}; cols.forEach(function (c, i) { ix[c] = i; });
    var defs = [['symbol', 'Symbol', 's'], ['direction', 'Side', 's'], ['entry_date', 'Entry', 's'], ['exit_date', 'Exit', 's'], ['qty', 'Qty', 'i'],
      ['entry_value_paise', 'Entry value', 'p'], ['exit_value_paise', 'Exit value', 'p'], ['gross_pnl_paise', 'Gross P&L', 'p'],
      ['fees_paise', 'Fees', 'p'], ['net_pnl_paise', 'Net P&L', 'pp'], ['return', 'Return', 'r']];
    var state = { col: null, dir: 1, all: false }, LIMIT = 20;
    var tbl = h('table', { id: 'trades-table' }), more = h('div', { class: 'row' });
    function cell(d, v) {
      if (v === null) return h('td', { class: 'n' }, 'n/a');
      var cls = 'n', txt;
      if (d[2] === 's') { return h('td', null, v); }
      if (d[2] === 'i') txt = int(v);
      else if (d[2] === 'p') txt = inr(v);
      else if (d[2] === 'pp') { txt = (v > 0 ? '+' : '') + inr(v); cls += v > 0 ? ' pos' : v < 0 ? ' neg' : ''; }
      else { txt = spct(v, 2); cls += v > 0 ? ' pos' : v < 0 ? ' neg' : ''; }
      return h('td', { class: cls }, txt);
    }
    function draw() {
      var view = rows.slice();
      if (state.col !== null) {
        var c = ix[state.col];
        view.sort(function (a, b) {
          var x = a[c], y = b[c];
          if (x === y) return 0;
          if (x === null) return 1; if (y === null) return -1;
          return (x < y ? -1 : 1) * state.dir;
        });
      }
      var shown = state.all ? view : view.slice(0, LIMIT);
      tbl.textContent = '';
      tbl.appendChild(h('thead', null, h('tr', null, defs.map(function (d) {
        var th = h('th', { class: 'sortable ' + (d[2] === 's' ? '' : 'n'), tabindex: 0, 'aria-sort': state.col === d[0] ? (state.dir > 0 ? 'ascending' : 'descending') : 'none' }, d[1]);
        function go() { if (state.col === d[0]) state.dir = -state.dir; else { state.col = d[0]; state.dir = d[2] === 's' ? 1 : -1; } draw(); }
        th.addEventListener('click', go);
        th.addEventListener('keydown', function (ev) { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); go(); } });
        return th;
      }))));
      tbl.appendChild(h('tbody', null, shown.map(function (r) { return h('tr', null, defs.map(function (d) { return cell(d, r[ix[d[0]]]); })); })));
      more.textContent = '';
      if (rows.length > LIMIT)
        more.appendChild(h('button', { type: 'button', onclick: function () { state.all = !state.all; draw(); } }, state.all ? 'Show first ' + LIMIT : 'Show all ' + int(rows.length) + ' trades'));
      more.appendChild(h('span', { class: 'muted' }, 'Showing ' + int(shown.length) + ' of ' + int(rows.length) + ' closed round trips. Click a header to sort.'));
    }
    var open = D.open_positions;
    var sec = h('section', { class: 'card', id: 'trades' }, h('h2', null, 'Trades'),
      rows.length ? [h('div', { class: 'scroll' }, tbl), more] : h('p', { class: 'muted' }, 'There were no closed trades.'),
      open.length ? [h('h3', null, 'Open positions at the end of the run'), kv(open.map(function (p) { return [p.symbol, int(p.qty) + ' shares', 'not in the closed-trade statistics']; }))] : null);
    return { el: sec, after: function () { if (rows.length) draw(); } };
  }

  function feesSection(D) {
    var F = D.fees, total = F.total_paise;
    var max = 0; F.components.forEach(function (c) { max = Math.max(max, c.paise); });
    var bars = F.components.map(function (c) {
      return h('div', { class: 'b' }, h('span', { class: 'sec' }, c.label),
        h('span', { class: 'bar' }, h('i', { style: 'width:' + (max > 0 ? (c.paise / max * 100).toFixed(1) : 0) + '%' })),
        h('span', { class: 'amt' }, inr(c.paise) + (c.share === null ? '' : '  ' + pct(c.share, 1))));
    });
    var sym = F.by_symbol.length ? h('div', { class: 'scroll' }, h('table', null, h('thead', null, h('tr', null, h('th', null, 'Symbol'), h('th', { class: 'n' }, 'Fills'), h('th', { class: 'n' }, 'Traded value'), h('th', { class: 'n' }, 'Fees'))),
      h('tbody', null, F.by_symbol.map(function (s) { return h('tr', null, h('td', null, s.symbol), h('td', { class: 'n' }, int(s.fills)), h('td', { class: 'n' }, inr(s.notional_paise)), h('td', { class: 'n' }, inr(s.paise))); })))) : null;
    var t = D.summary.trades;
    return h('section', { class: 'card', id: 'fees' }, h('h2', null, 'Fee breakdown'),
      h('div', { class: 'two' }, h('div', { class: 'bars' }, bars, h('div', { class: 'b' }, h('span', null, 'Total'), h('span'), h('span', { class: 'amt' }, inr(total)))),
        h('div', null, kv([['Fees as % of gross P&L', t.fees_pct_gross_pnl === null ? 'n/a (gross P&L not positive)' : pct(t.fees_pct_gross_pnl, 2)], ['Fees as % of initial capital', pct(t.fees_pct_initial_capital, 3)],
          ['Buy-side fees', inr(F.by_side.buy_paise)], ['Sell-side fees', inr(F.by_side.sell_paise)], ['Fills', int(t.num_fills)]]), sym)),
      h('p', { class: 'note' }, 'Cost model: ' + (D.run.config.costs_source || 'see config') + '. Percentages are of the total fee bill.'));
  }

  function configSection(D) {
    var R = D.run, files = R.data.traded.concat(R.data.benchmark ? [R.data.benchmark] : []);
    var ftab = h('div', { class: 'scroll' }, h('table', null, h('thead', null, h('tr', null, ['Symbol', 'File', 'Bars', 'First', 'Last', 'Null rows skipped', 'Dates excluded'].map(function (x, i) { return h('th', { class: i > 1 ? 'n' : '' }, x); }))),
      h('tbody', null, files.map(function (f, i) {
        return h('tr', null, h('td', null, f.symbol + (R.data.benchmark && i === files.length - 1 ? ' (benchmark)' : '')), h('td', null, f.path), h('td', { class: 'n' }, int(f.bars)), h('td', { class: 'n' }, f.first_date), h('td', { class: 'n' }, f.last_date),
          h('td', { class: 'n' }, int(f.skipped_null_rows)), h('td', { class: 'n' }, int(f.excluded_dates.length)));
      }))));
    var w = R.window, o = R.orders;
    return h('section', { class: 'card', id: 'config' }, h('h2', null, 'Configuration used'),
      kv([['Strategy', R.config.strategy.name], ['Evaluation window', w.first_evaluation_date + ' to ' + w.last_evaluation_date + ' (' + int(w.evaluation_days) + ' days)'],
        ['Warm-up days before the window', int(w.warmup_days) + ' (' + int(w.warmup_orders_discarded) + ' orders discarded)'], ['Price basis', R.config.data.prices],
        ['Orders', int(o.fills) + ' fills, ' + int(o.expired) + ' expired, ' + int(o.cancelled) + ' cancelled, ' + int(o.pending_at_end) + ' pending at end'],
        ['Rejected orders', Object.keys(o.rejected_by_reason).length ? Object.keys(o.rejected_by_reason).map(function (k) { return k + ': ' + o.rejected_by_reason[k]; }).join(', ') : 'none'],
        ['Version', R.tool + ' ' + R.version]]),
      h('h3', null, 'Data files'), ftab,
      h('p', { class: 'note' }, 'Directories are shortened to file names in this report; the full resolved paths are in run.json.'),
      h('details', null, h('summary', null, 'Resolved configuration (JSON)'), h('pre', { class: 'cfg', id: 'cfg-json' }, JSON.stringify(R.config, null, 2))));
  }

  function warningsBlock(list) {
    if (!list || !list.length) return null;
    return h('div', { class: 'callout', id: 'warnings' }, h('strong', null, list.length === 1 ? 'Warning' : 'Warnings'), h('ul', null, list.map(function (w) { return h('li', null, w); })));
  }

  function pageFrame(D, subtitle, body) {
    return [
      h('div', { class: 'banner', role: 'note' }, DISCLAIMER),
      h('div', { class: 'wrap' },
        h('header', { class: 'top' },
          D.context ? h('div', { class: 'crumb' }, h('a', { href: D.context.link }, D.context.link_text)) : null,
          h('h1', null, D.title), h('div', { class: 'sub' }, subtitle)),
        body,
        h('footer', null, h('p', null, DISCLAIMER + ' Research and education software: results come from a simulation with simplified costs and fills, on the data files named above.'),
          h('p', null, 'Generated by ' + D.run_tool + '. This file is self-contained: it makes no network requests.')))];
  }

  /* ----------------------------------------------------------------------------------------------- run report page */
  function renderRun(D) {
    var after = [];
    function part(p) { if (p.after) after.push(p.after); return p.el; }
    var ctx = D.context ? h('div', { class: 'callout info', id: 'context' }, h('strong', null, D.context.title + ' '), D.context.text) : null;
    var body = [ctx, warningsBlock(D.run.warnings), tilesSection(D), metricsSection(D), part(equitySection(D)), part(drawdownSection(D)), monthlySection(D), part(tradesSection(D)), feesSection(D), configSection(D)];
    var sub = D.run.config.strategy.name + ' · ' + D.run.window.first_evaluation_date + ' to ' + D.run.window.last_evaluation_date + ' · start capital ' + inr(D.summary.equity.start_equity_paise);
    return { nodes: pageFrame(D, sub, body), after: after };
  }

  /* --------------------------------------------------------------------------------------------- sweep report page */
  var METRIC_LABEL = { total_return: 'Total return', cagr: 'CAGR', annualised_volatility: 'Volatility', sharpe: 'Sharpe', sortino: 'Sortino', max_drawdown: 'Max drawdown',
    calmar: 'Calmar', num_trades: 'Trades', win_rate: 'Win rate', profit_factor: 'Profit factor', turnover_annual: 'Turnover / yr', fees_pct_initial_capital: 'Fees % of capital' };
  var METRIC_FMT = { total_return: spct, cagr: spct, annualised_volatility: pct, sharpe: num, sortino: num, max_drawdown: pct, calmar: num, num_trades: int, win_rate: pct,
    profit_factor: num, turnover_annual: num, fees_pct_initial_capital: function (v) { return pct(v, 3); } };

  function renderSweep(D) {
    var cols = D.columns, S = D.summary, axes = D.axes, hasOos = cols.indexOf('oos_sharpe') >= 0;
    var rows = D.rows.map(function (r) { var o = {}; cols.forEach(function (c, i) { o[c] = r[i]; }); return o; });
    var bestRow = rows.filter(function (r) { return r.selected === 1; })[0] || null;
    var selMetric = S.selection_metric;
    var axVals = {};
    axes.forEach(function (a) {
      var seen = [];
      rows.forEach(function (r) { if (seen.indexOf(r[a]) < 0) seen.push(r[a]); });
      if (seen.every(function (v) { return typeof v === 'number'; })) seen.sort(function (p, q) { return p - q; });
      axVals[a] = seen;
    });
    var st = { metric: selMetric, xa: axes.length > 1 ? axes[axes.length - 1] : axes[0], ya: axes.length > 1 ? axes[0] : null, hold: {} };
    axes.forEach(function (a) { st.hold[a] = bestRow ? bestRow[a] : axVals[a][0]; });

    function fmt(metric, v) { return v === null || v === undefined ? 'n/a' : (METRIC_FMT[metric] || num)(v); }
    function axLabel(v) { return typeof v === 'number' ? String(v) : String(v); }

    function heat(prefix, title, note) {
      var tbl = h('table', { class: 'heat', 'data-window': prefix });
      var key = prefix + '_' + st.metric, lo = Infinity, hi = -Infinity;
      var xs = axVals[st.xa], ys = st.ya ? axVals[st.ya] : [null];
      function find(xv, yv) {
        return rows.filter(function (r) {
          if (r[st.xa] !== xv) return false;
          if (st.ya && r[st.ya] !== yv) return false;
          return axes.every(function (a) { return a === st.xa || a === st.ya || r[a] === st.hold[a]; });
        })[0] || null;
      }
      ys.forEach(function (yv) { xs.forEach(function (xv) { var r = find(xv, yv); if (r && r.status === 'ok' && r[key] !== null) { lo = Math.min(lo, r[key]); hi = Math.max(hi, r[key]); } }); });
      tbl.appendChild(h('thead', null, h('tr', null, h('th', { class: 'ax' }, (st.ya ? st.ya + ' ↓  ' : '') + st.xa + ' →'), xs.map(function (x) { return h('th', null, axLabel(x)); }))));
      var body = h('tbody');
      ys.forEach(function (yv) {
        var tr = h('tr', null, h('th', { class: 'yr' }, yv === null ? '' : axLabel(yv)));
        xs.forEach(function (xv) {
          var r = find(xv, yv), td;
          if (!r) { tr.appendChild(h('td', { class: 'empty' }, '')); return; }
          var v = r[key];
          if (r.status !== 'ok' || v === null) {
            td = h('td', { class: 'hatch', tabindex: 0 }, r.status !== 'ok' ? r.status : 'n/a');
          } else {
            var c = sequentialColor(v, lo, hi);
            td = h('td', { style: 'background:' + c.bg + ';color:' + c.fg, tabindex: 0 }, fmt(st.metric, v));
          }
          if (r.selected === 1) td.className = (td.className ? td.className + ' ' : '') + 'selcell';
          tipOn(td, (function (r, v) { return function () {
            var rowsT = [{ label: 'run', value: String(r.run_id) }].concat(axes.map(function (a) { return { label: a, value: String(r[a]) }; }));
            rowsT.push({ label: METRIC_LABEL[st.metric], value: fmt(st.metric, v) });
            if (r.status !== 'ok') rowsT.push({ label: 'status', value: r.status + (r.message ? ': ' + r.message : '') });
            if (r.selected === 1) rowsT.push({ label: 'selected', value: 'yes (best in-sample)' });
            return { title: prefix === 'is' ? 'In-sample' : 'Out-of-sample', rows: rowsT };
          }; })(r, v));
          tr.appendChild(td);
        });
        body.appendChild(tr);
      });
      tbl.appendChild(body);
      var ramp = isFinite(lo) ? h('div', { class: 'ramp' }, fmt(st.metric, lo), h('i', { style: 'background:linear-gradient(90deg,' + rgb(SEQ_LO) + ',' + rgb(SEQ_HI) + ')' }), fmt(st.metric, hi)) : h('div', { class: 'ramp' }, 'no defined values');
      return h('div', null, h('h3', { style: 'margin-top:0' }, title), h('div', { class: 'scroll' }, tbl), ramp, h('p', { class: 'note' }, note));
    }

    var heatHost = h('div', { class: 'two', id: 'heatmaps' });
    function drawHeat() {
      heatHost.textContent = '';
      heatHost.appendChild(heat('is', 'In-sample (used for selection)', 'Outlined cell = the selected combination. Hatched = invalid, errored or undefined. Colour scale is per heatmap (darkest = lowest, lightest = highest).'));
      if (hasOos) heatHost.appendChild(heat('oos', 'Out-of-sample (diagnostic only)', 'Never used to choose a combination. Its colour scale is its own, so colours are not comparable with the in-sample map.'));
    }
    var metricSel = h('select', { id: 'metric-sel', 'aria-label': 'Metric' }, Object.keys(METRIC_LABEL).map(function (m) { return h('option', { value: m, selected: m === st.metric }, METRIC_LABEL[m]); }));
    metricSel.addEventListener('change', function () { st.metric = metricSel.value; drawHeat(); });
    var ctl = [h('label', null, 'Metric '), metricSel];
    if (axes.length > 1) {
      var xs = h('select', { id: 'x-sel', 'aria-label': 'Columns' }, axes.map(function (a) { return h('option', { value: a, selected: a === st.xa }, a); }));
      var ys = h('select', { id: 'y-sel', 'aria-label': 'Rows' }, axes.map(function (a) { return h('option', { value: a, selected: a === st.ya }, a); }));
      var holds = h('span', { class: 'row', style: 'margin:0' });
      function drawHolds() {
        holds.textContent = '';
        axes.forEach(function (a) {
          if (a === st.xa || a === st.ya) return;
          var sel = h('select', { 'aria-label': 'Hold ' + a }, axVals[a].map(function (v) { return h('option', { value: String(v), selected: v === st.hold[a] }, String(v)); }));
          sel.addEventListener('change', function () { st.hold[a] = axVals[a][sel.selectedIndex]; drawHeat(); });
          holds.appendChild(h('label', null, 'hold ' + a + ' = ')); holds.appendChild(sel);
        });
      }
      xs.addEventListener('change', function () { st.xa = xs.value; if (st.ya === st.xa) { st.ya = axes.filter(function (a) { return a !== st.xa; })[0]; ys.value = st.ya; } drawHolds(); drawHeat(); });
      ys.addEventListener('change', function () { st.ya = ys.value; if (st.xa === st.ya) { st.xa = axes.filter(function (a) { return a !== st.ya; })[0]; xs.value = st.xa; } drawHolds(); drawHeat(); });
      ctl.push(h('label', null, 'Columns '), xs, h('label', null, 'Rows '), ys, holds);
      drawHolds();
    }
    drawHeat();

    // ---- in-sample vs out-of-sample table, sorted by the in-sample selection metric
    var tcols = [['run_id', 'Run', 'i'], ].concat(axes.map(function (a) { return [a, a, 'a']; })).concat([
      ['is_' + selMetric, 'IS ' + METRIC_LABEL[selMetric], 'm'], ['is_cagr', 'IS CAGR', 'm'], ['is_max_drawdown', 'IS max DD', 'm'], ['is_num_trades', 'IS trades', 'm']]);
    if (hasOos) tcols = tcols.concat([['oos_' + selMetric, 'OOS ' + METRIC_LABEL[selMetric], 'm'], ['oos_cagr', 'OOS CAGR', 'm'], ['oos_max_drawdown', 'OOS max DD', 'm'], ['oos_num_trades', 'OOS trades', 'm']]);
    var ranked = rows.slice().sort(function (p, q) {
      var a = p['is_' + selMetric], b = q['is_' + selMetric];
      if (a === b) return p.run_id - q.run_id;
      if (a === null) return 1; if (b === null) return -1;
      return b - a;
    });
    var state = { all: false }, LIMIT = 20, tbl = h('table', { id: 'sweep-table' }), moreRow = h('div', { class: 'row' });
    function metricName(col) { return col.replace(/^(is|oos)_/, ''); }
    function drawTable() {
      var shown = state.all ? ranked : ranked.slice(0, LIMIT);
      tbl.textContent = '';
      tbl.appendChild(h('thead', null, h('tr', null, tcols.map(function (c) { return h('th', { class: c[2] === 'a' ? '' : 'n' }, c[1]); }).concat([h('th', null, 'Status')]))));
      tbl.appendChild(h('tbody', null, shown.map(function (r) {
        return h('tr', { class: r.selected === 1 ? 'sel' : '' }, tcols.map(function (c) {
          var v = r[c[0]];
          if (c[2] === 'a') return h('td', null, String(v));
          if (c[2] === 'i') return h('td', { class: 'n' }, String(v) + (r.selected === 1 ? ' ★' : ''));
          return h('td', { class: 'n' }, fmt(metricName(c[0]), v));
        }).concat([h('td', { class: 'muted' }, r.status === 'ok' ? (r.is_num_trades !== null && r.is_num_trades < S.min_trades ? 'not eligible: fewer than ' + S.min_trades + ' in-sample trades' : '') : r.status + (r.message ? ': ' + r.message : ''))]));
      })));
      moreRow.textContent = '';
      if (ranked.length > LIMIT) moreRow.appendChild(h('button', { type: 'button', onclick: function () { state.all = !state.all; drawTable(); } }, state.all ? 'Show first ' + LIMIT : 'Show all ' + int(ranked.length) + ' combinations'));
      moreRow.appendChild(h('span', { class: 'muted' }, 'Sorted by in-sample ' + METRIC_LABEL[selMetric] + ', best first; ★ = selected (the best ELIGIBLE row: needs at least ' + S.min_trades + ' in-sample trades).'));
    }
    drawTable();

    var W = S.windows, nComb = S.grid.n_combinations, b = S.best;
    var notes = [
      'The best of ' + int(nComb) + ' in-sample results is biased upward: the more combinations are tried, the better the best one looks by chance alone.',
      'Selection rule: ' + S.selection_rule + '.',
      W.mode === 'single_split' ? 'In-sample window ' + W.in_sample.start + ' to ' + W.in_sample.end + ' (' + int(W.in_sample.days) + ' days); out-of-sample ' + W.out_of_sample.start + ' to ' + W.out_of_sample.end + ' (' + int(W.out_of_sample.days) + ' days), run with fresh capital after the in-sample data was cut off. A single split is not a rolling walk-forward.'
        : 'No out-of-sample split was configured: the selected result is reported on the same data it was chosen on.'
    ].concat(S.notes.filter(function (n) { return n.indexOf('biased upward') < 0 && n.indexOf('single in-sample') < 0; }));
    var warn = S.warnings && S.warnings.length ? h('div', { class: 'callout', id: 'warnings' }, h('strong', null, S.warnings.length === 1 ? 'Warning' : 'Warnings'), h('ul', null, S.warnings.map(function (w) { return h('li', null, w); }))) : null;
    var caution = h('div', { class: 'callout info', id: 'bias-note' }, h('strong', null, 'Read this first. '), notes[0] + ' ' + (W.mode === 'single_split' ? 'Judge the choice by the out-of-sample columns, and remember that repeating this exercise until the out-of-sample result looks good is itself overfitting.' : 'There is no out-of-sample check in this report.'));

    function tile(k, v, d) { return h('div', { class: 'tile' }, h('div', { class: 'k' }, k), h('div', { class: 'v' }, v), d ? h('div', { class: 'd' }, d) : null); }
    var tiles = h('div', { class: 'tiles' },
      tile('Combinations', int(nComb), int(S.counts.ok) + ' ran, ' + int(S.counts.invalid) + ' invalid, ' + int(S.counts.error) + ' failed'),
      tile('Selection metric', METRIC_LABEL[selMetric] || selMetric, 'min ' + int(S.min_trades) + ' trades'),
      tile('Eligible', int(S.counts.eligible_for_selection), 'of ' + int(nComb)),
      tile('Selected run', b ? String(b.run_id) : 'none', b ? JSON.stringify(b.params) : null),
      b && b.out_of_sample_rank ? tile('OOS rank of selected', int(b.out_of_sample_rank.rank) + ' of ' + int(b.out_of_sample_rank.of), 'by ' + selMetric + ', out-of-sample') : null);

    var bestCard = b ? h('section', { class: 'card', id: 'best' }, h('h2', null, 'Selected combination'),
      h('div', { class: 'two' },
        h('div', null, h('h3', { style: 'margin-top:0' }, 'In-sample'), kv([['Total return', spct(b.in_sample.total_return)], ['CAGR', spct(b.in_sample.cagr)], ['Sharpe', num(b.in_sample.sharpe)], ['Max drawdown', pct(b.in_sample.max_drawdown)], ['Trades', int(b.in_sample.num_trades)]])),
        h('div', null, h('h3', { style: 'margin-top:0' }, 'Out-of-sample'), b.out_of_sample ? kv([['Total return', spct(b.out_of_sample.total_return)], ['CAGR', spct(b.out_of_sample.cagr)], ['Sharpe', num(b.out_of_sample.sharpe)], ['Max drawdown', pct(b.out_of_sample.max_drawdown)], ['Trades', int(b.out_of_sample.num_trades)]]) : h('p', { class: 'muted' }, 'No out-of-sample run.'))),
      D.best_report ? h('p', null, h('a', { href: D.best_report, id: 'best-link' }, 'Open the full report for the selected parameters'), W.mode === 'single_split' ? ' (this is the out-of-sample run, started with fresh capital on ' + W.out_of_sample.start + ').' : ' (whole window).') : null) : null;

    var body = [warn, caution, tiles,
      h('section', { class: 'card', id: 'heat' }, h('h2', null, 'Parameter heatmap'), h('div', { class: 'row' }, ctl), heatHost),
      h('section', { class: 'card', id: 'compare' }, h('h2', null, hasOos ? 'In-sample vs out-of-sample' : 'All combinations'), h('div', { class: 'scroll' }, tbl), moreRow),
      bestCard,
      h('section', { class: 'card', id: 'notes' }, h('h2', null, 'Notes'), h('ul', null, notes.map(function (n) { return h('li', null, n); })))];
    var sub = S.strategy + ' · ' + int(nComb) + ' combinations over ' + axes.join(', ') + (W.mode === 'single_split' ? ' · in-sample / out-of-sample split' : ' · no split');
    return { nodes: pageFrame(D, sub, body.filter(Boolean)), after: [] };
  }

  /* ------------------------------------------------------------------------------------------------------- boot */
  function render(D) {
    var app = document.getElementById('app');
    var out = D.kind === 'sweep' ? renderSweep(D) : renderRun(D);
    app.textContent = '';
    out.nodes.forEach(function (n) { app.appendChild(n); });
    out.after.forEach(function (f) { f(); });
  }

  function fail(msg) {
    var b = document.body;
    if (!b.getAttribute('data-error')) b.setAttribute('data-error', String(msg).slice(0, 600));
    var app = document.getElementById('app');
    if (app) app.appendChild(h('div', { class: 'err' }, 'The report could not be drawn: ' + String(msg).slice(0, 600) + '\nThe data is still in the page source (script#bt-data).'));
  }

  function boot() {
    window.addEventListener('error', function (ev) { fail(ev.message || 'script error'); });
    try {
      var D = JSON.parse(document.getElementById('bt-data').textContent);
      render(D);
      document.body.setAttribute('data-rendered', '1');
    } catch (e) { fail(e && e.stack ? e.stack : e); }
  }

  var api = { inr: inr, inrAxis: inrAxis, groupIndian: groupIndian, pct: pct, spct: spct, num: num, niceTicks: niceTicks, logTicks: logTicks, timeTicks: timeTicks,
    dayNum: dayNum, dayIso: dayIso, divergingColor: divergingColor, sequentialColor: sequentialColor };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  if (typeof document !== 'undefined' && document.getElementById && document.getElementById('bt-data')) {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot); else boot();
  }
})(typeof window !== 'undefined' ? window : this);
