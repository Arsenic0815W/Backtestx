// Node tests for the pure formatting functions in src/report_assets/report.js (skipped by build.ps1 when node is absent).
//   node tools/test_report_js.js
'use strict';
const path = require('path');
const assert = require('assert');
const R = require(path.join(__dirname, '..', 'src', 'report_assets', 'report.js'));

let n = 0;
function eq(actual, expected, what) { assert.strictEqual(actual, expected, what + ': got ' + actual + ', expected ' + expected); n++; }

// inr: integer paise -> rupees with Indian digit grouping, integer arithmetic only
eq(R.inr(0), '₹0.00', 'zero');
eq(R.inr(5), '₹0.05', '5 paise');
eq(R.inr(99999), '₹999.99', '999.99');
eq(R.inr(100000), '₹1,000.00', '1,000');
eq(R.inr(12345678), '₹1,23,456.78', '1,23,456.78');
eq(R.inr(1234567890), '₹1,23,45,678.90', '1,23,45,678.90');
eq(R.inr(-12345678), '-₹1,23,456.78', 'negative: sign before the symbol');
eq(R.inr(-5), '-₹0.05', 'negative paise');
eq(R.inr(100000000), '₹10,00,000.00', 'ten lakh');
eq(R.inr(9007199254740990), '₹9,00,71,99,25,47,409.90', 'near 2^53 stays exact');
eq(R.inr(12345678, false), '1,23,456.78', 'no symbol');
eq(R.inr(null), 'n/a', 'null');
eq(R.groupIndian('1234567'), '12,34,567', 'group');
eq(R.groupIndian('123'), '123', 'short group');
// axis labels
eq(R.inrAxis(1000000), '₹10L', 'axis 10 lakh');
eq(R.inrAxis(12000000), '₹1.2Cr', 'axis 1.2 crore');
eq(R.inrAxis(45000), '₹45,000', 'axis thousands');
// percentages
eq(R.pct(0.12345), '12.35%', 'pct');
eq(R.pct(null), 'n/a', 'pct null');
eq(R.spct(0.05), '+5.00%', 'signed +');
eq(R.spct(-0.05), '-5.00%', 'signed -');
eq(R.spct(0), '0.00%', 'signed zero');
eq(R.num(null), 'n/a', 'num null');
// dates
eq(R.dayIso(R.dayNum('2024-02-29')), '2024-02-29', 'leap day round trip');
eq(R.dayNum('1970-01-02'), 1, 'epoch + 1');
// ticks
const t = R.niceTicks(0, 100, 5);
assert.deepStrictEqual(t, [0, 20, 40, 60, 80, 100]); n++;
assert.ok(R.logTicks(1e6, 1.3e6).length >= 2); n++;
const tt = R.timeTicks(R.dayNum('2022-01-01'), R.dayNum('2024-12-31'), 800);
assert.ok(tt.length >= 2 && tt.length <= 12); n++;
// colour scales are well-formed and symmetric at the neutral point
const c0 = R.divergingColor(0, 0.1), cp = R.divergingColor(0.1, 0.1), cn = R.divergingColor(-0.1, 0.1);
assert.ok(/^rgb\(/.test(c0.bg) && cp.bg !== cn.bg && cp.bg !== c0.bg); n++;
assert.ok(/^rgb\(/.test(R.sequentialColor(1, 1, 1).bg)); n++;
console.log('report.js: ' + n + ' checks passed');
