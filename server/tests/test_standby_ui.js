// Synthetic contract fixtures, not provider qualification. Exercise operator text.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(require('node:path').join(__dirname, '../ui/app.js'), 'utf8');
const start = source.indexOf('  function standbyRttDisplay(src) {');
const end = source.indexOf('  function tilesFor(src) {', start);
assert(start >= 0 && end > start);
const context = vm.createContext({state: {lastSnapshotSteadyMs: 0}, performance: {now: () => 0}, fmtAge: s => `${s}s`, fmtDuration: s => `${s}s`});
vm.runInContext(source.slice(start, end), context);
const display = m => context.standbyRttDisplay({standby_rtt: m});
assert.equal(display().value, '—');
assert.match(display({availability: 'read_failed'}).note, /read failed/);
assert.match(display({availability: 'available', negotiated: false, last_outcome: 'none'}).note, /not negotiated/);
assert.match(display({availability: 'available', negotiated: true, admitted: false, last_outcome: 'capacity'}).note, /capacity limited/);
const measured = {availability: 'available', negotiated: true, admitted: true, observed: true,
  fresh: true, fresh_at_capture: true, rtt_seconds: 0.025, age_seconds: 1, capture_age_seconds: 1,
  last_outcome: 'measured', samples: '18446744073709551615'};
assert.equal(display(measured).value, '0.025s');
assert.match(display(measured).note, /2s old · fresh/);
assert.match(display(measured).note, /18446744073709551615 accepted samples/);
assert.match(display({...measured, capture_age_seconds: 2.001}).note, /stale/);
assert.match(display({...measured, age_seconds: null, fresh: false}).note, /age unavailable/);
assert.match(display({...measured, terminal: true, fresh: false}).note, /retained terminal/);
for (const outcome of ['pending', 'measured', 'timeout', 'send_skipped', 'paused', 'id_exhausted']) {
  const shown = display({...measured, last_outcome: outcome});
  assert.equal(shown.value, '0.025s');
  assert.match(shown.note, /last attempt:/);
  assert.match(shown.note, /separate from DATA RTT and health/);
  assert.doesNotMatch(shown.note, /currently paused|healthy|unhealthy/);
}
assert.equal(display({availability: 'available', negotiated: true, admitted: true,
  observed: false, last_outcome: 'pending'}).value, '—');

context.performance.now = () => 1500;
assert.match(display(measured).note, /3.5s old · stale/);
console.log('Standby RTT display contract passed');
