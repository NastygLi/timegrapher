// End-to-end check: synthetic watch audio -> Detector -> Analyzer.
// Run: node test/analysis.test.js
const TG = require('../web/analysis.js');

function simulate(watchOpts, seconds, anaOpts = {}) {
  const w = new TG.SyntheticWatch(watchOpts);
  const d = new TG.Detector(48000, { refractoryMs: 25 });
  const a = new TG.Analyzer(anaOpts);
  const block = 512;
  let ticks = 0;
  for (let i = 0; i < (48000 * seconds) / block; i++) {
    for (const t of d.process(w.render(block))) { a.addTick(t); ticks++; }
  }
  return { r: a.result, bph: a.bph, ticks };
}

let failed = 0;
function check(name, got, want, tol) {
  const ok = got != null && Math.abs(got - want) <= tol;
  if (!ok) failed++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: got ${got == null ? got : got.toFixed(3)}, want ${want} ±${tol}`);
}

const cases = [
  { bph: 28800, rate: 5, beatErrorMs: 0.6, amplitude: 275 },
  { bph: 21600, rate: -12, beatErrorMs: 1.8, amplitude: 230 },
  { bph: 18000, rate: 30, beatErrorMs: 0.2, amplitude: 200 },
  { bph: 36000, rate: -2, beatErrorMs: 0.4, amplitude: 300 },
  { bph: 28800, rate: 8, beatErrorMs: 1.0, amplitude: 260, missProb: 0.1, noise: 0.002 },
  // like the first real watch tested: stray clicks between beats, and the
  // unlock sometimes too quiet so the trigger fires on the drop instead
  { bph: 21600, rate: 5, beatErrorMs: 0.4, amplitude: 215, strayPerSec: 1.5, quietAProb: 0.15 },
];
for (const c of cases) {
  console.log(`\n-- ${JSON.stringify(c)}`);
  const { r, bph, ticks } = simulate(c, 30);
  check('bph', bph, c.bph, 0);
  if (!r) { failed++; console.log('FAIL no result, ticks=' + ticks); continue; }
  check('rate s/d', r.rate, c.rate, 1.0);
  check('beat error ms', r.beatError, c.beatErrorMs, 0.1);
  check('amplitude deg', r.amp, c.amplitude, 15);
  console.log(`     jitter ${r.jitterMs.toFixed(3)} ms, ${r.count} ticks in window`);
}

// ADC clock 20 ppm fast -> watch looks 1.73 s/d slow unless corrected
console.log('\n-- clock ppm correction');
{
  const c = { bph: 28800, rate: 0, beatErrorMs: 0.3, amplitude: 280, clockPpm: 20 };
  check('uncorrected rate', simulate(c, 30).r.rate, -1.728, 0.5);
  check('corrected rate', simulate(c, 30, { ppm: 20 }).r.rate, 0, 0.5);
}

// Serial framing: a tick frame split across chunks with garbage in between
console.log('\n-- serial framing');
{
  const payload = new Uint8Array(28 + 4);
  payload[0] = 1; payload[1] = 1; payload[2] = 2;   // type, ver, n=2
  new DataView(payload.buffer).setFloat64(8, 123456.75, true);
  const frame = l => { const f = new Uint8Array(6 + l.length); f.set([0xA5, 0x5A, 1, l.length & 255, l.length >> 8]); f.set(l, 5); f[5 + l.length] = l.reduce((a, b) => (a + b) & 255, 0); return f; };
  const good = frame(payload), corrupt = frame(payload); corrupt[20] ^= 1;
  const stream = new Uint8Array([7, 0xA5, 3, ...corrupt, ...good, 9, 9]);
  const fp = new TG.FrameParser(); const got = [];
  for (let i = 0; i < stream.length; i += 13) got.push(...fp.push(stream.subarray(i, i + 13)));
  check('frames decoded', got.length, 1, 0);
  check('onset', got.length && TG.decodeTick(got[0].payload).onset, 123456.75, 0);
  check('corrupt frames counted', fp.bad, 1, 0);
}

console.log(failed ? `\n${failed} check(s) failed` : '\nall checks passed');
process.exit(failed ? 1 : 0);
