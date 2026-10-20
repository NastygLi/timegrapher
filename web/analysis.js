// Timegrapher analysis: tick detection (mirror of the firmware), rate / beat
// error / amplitude estimation, and a synthetic watch for testing.
// Works in the browser (window.TG) and in Node (module.exports).
(function (root) {
  'use strict';

  const STANDARD_BPH = [12000, 14400, 18000, 19800, 21600, 25200, 28800, 36000, 43200];

  function median(a) {
    if (!a.length) return NaN;
    const s = Array.from(a).sort((x, y) => x - y);
    const m = s.length >> 1;
    return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
  }

  // ------------------------------------------------------------ Detector
  // Same algorithm as loop1() in pico_timegrapher.ino (and audioTask() in the
  // ESP32 sketch). Used by the simulator and the offline tests; keep in sync.
  class Biquad {
    constructor() { this.b0 = 1; this.b1 = this.b2 = this.a1 = this.a2 = this.z1 = this.z2 = 0; }
    highpass(fc, q, fs) {
      const w0 = 2 * Math.PI * fc / fs, c = Math.cos(w0), alpha = Math.sin(w0) / (2 * q), a0 = 1 + alpha;
      this.b0 = (1 + c) / 2 / a0; this.b1 = -(1 + c) / a0; this.b2 = this.b0;
      this.a1 = -2 * c / a0; this.a2 = (1 - alpha) / a0;
    }
    run(x) {
      const y = this.b0 * x + this.z1;
      this.z1 = this.b1 * x - this.a1 * y + this.z2;
      this.z2 = this.b2 * x - this.a2 * y;
      return y;
    }
  }

  class Detector {
    constructor(fs = 48000, cfg = {}) {
      this.fs = fs;
      this.PRE = Math.round(fs * 0.002);
      this.POST = Math.round(fs * 0.038);
      this.SNAP = this.PRE + this.POST;
      this.RING = 4096;
      this.ring = new Float32Array(this.RING);
      this.cfg = { hpf: 2000, sens: 6, minThr: 0.0002, refractoryMs: 40 };
      this.hp1 = new Biquad(); this.hp2 = new Biquad();
      this.setConfig(cfg);
      // shift-based constants, as in the fixed-point Pico firmware
      this.envDecay = 1 - 1 / 32 - 1 / 128;   // ~0.5 ms release
      this.floorDown = 1 / 1024;              // ~21 ms
      this.floorUp = 1 / 131072;              // ~2.7 s
      this.env = 0; this.prevEnv = 0; this.floor = 1e-4;
      this.idx = 0; this.lastOnset = -1; this.pending = [];
    }
    setConfig(c) {
      Object.assign(this.cfg, c);
      this.hp1.highpass(this.cfg.hpf, 0.5412, this.fs);
      this.hp2.highpass(this.cfg.hpf, 1.3066, this.fs);
    }
    // Returns ticks completed during this block, in the same shape the
    // firmware sends: { onset, pre, env (Float32Array, peak = 1), peak, floor, thr }
    process(samples) {
      const out = [], R = this.RING - 1, refr = this.cfg.refractoryMs / 1000 * this.fs;
      const { sens, minThr } = this.cfg;
      for (let i = 0; i < samples.length; i++) {
        const y = this.hp2.run(this.hp1.run(samples[i]));
        const a = Math.abs(y), d = this.env * this.envDecay;
        this.env = a > d ? a : d;
        const env = this.env, idx = this.idx;
        this.ring[idx & R] = env;
        this.floor += (env < this.floor ? this.floorDown : this.floorUp) * (env - this.floor);
        const thr = Math.max(this.floor * sens, minThr);
        if (env >= thr && this.prevEnv < thr && (this.lastOnset < 0 || idx - this.lastOnset > refr) && idx > this.PRE + 4800) {
          this.lastOnset = idx;
          if (this.pending.length < 4)
            this.pending.push({ onsetIdx: idx, onset: idx - 1 + (thr - this.prevEnv) / (env - this.prevEnv), floor: this.floor, thr });
        }
        this.prevEnv = env;
        if (this.pending.length && idx >= this.pending[0].onsetIdx + this.POST - 1) {
          const p = this.pending.shift(), start = p.onsetIdx - this.PRE;
          const e = new Float32Array(this.SNAP);
          let peak = 1e-9;
          for (let k = 0; k < this.SNAP; k++) { e[k] = this.ring[(start + k) & R]; if (e[k] > peak) peak = e[k]; }
          // quantise like the firmware's u16 transport
          for (let k = 0; k < this.SNAP; k++) e[k] = Math.round(e[k] / peak * 65535) / 65535;
          out.push({ onset: p.onset, pre: this.PRE, env: e, peak, floor: p.floor, thr: p.thr });
        }
        this.idx++;
      }
      return out;
    }
  }

  // Decode the firmware's binary tick frame.
  function decodeTick(buf) {
    const dv = new DataView(buf);
    if (dv.getUint8(0) !== 1) return null;
    const n = dv.getUint16(2, true);
    const env = new Float32Array(n);
    for (let i = 0; i < n; i++) env[i] = dv.getUint16(28 + 2 * i, true) / 65535;
    return {
      pre: dv.getUint32(4, true), onset: dv.getFloat64(8, true),
      peak: dv.getFloat32(16, true), floor: dv.getFloat32(20, true), thr: dv.getFloat32(24, true), env,
    };
  }

  // Byte-stream framing used over USB serial by the Pico:
  //   A5 5A, u8 type, u16 len (LE), payload[len], u8 sum(payload) & 0xff
  // push() returns complete, checksum-valid frames as {type, payload: ArrayBuffer}.
  class FrameParser {
    constructor() { this.buf = new Uint8Array(0); this.bad = 0; }
    push(chunk) {
      const b = new Uint8Array(this.buf.length + chunk.length);
      b.set(this.buf); b.set(chunk, this.buf.length);
      const out = [];
      let i = 0;
      while (true) {
        while (i + 1 < b.length && !(b[i] === 0xA5 && b[i + 1] === 0x5A)) i++;
        if (b.length - i < 5) break;
        const type = b[i + 2], len = b[i + 3] | (b[i + 4] << 8);
        if (len > 16384) { i += 2; continue; }
        if (b.length - i < 6 + len) break;
        const payload = b.slice(i + 5, i + 5 + len);
        let sum = 0;
        for (let k = 0; k < len; k++) sum = (sum + payload[k]) & 0xff;
        if (sum === b[i + 5 + len]) { out.push({ type, payload: payload.buffer }); i += 6 + len; }
        else { this.bad++; i += 2; }
      }
      this.buf = b.slice(i);
      return out;
    }
  }

  // ------------------------------------------------------------ BPH detection
  // Score each standard rate by the share of intervals that land on an integer
  // number of beats. Missed ticks still fit (2T, 3T); among near-equal scores the
  // lowest BPH wins, since every multiple of a slow rate also fits a fast one.
  function detectBph(intervals) {
    const iv = intervals.filter(x => x > 0.04 && x < 2);
    if (iv.length < 8) return 0;
    const scores = STANDARD_BPH.map(bph => {
      const T = 3600 / bph;
      let ok = 0;
      for (const x of iv) { const k = x / T, r = Math.round(k); if (r >= 1 && r <= 4 && Math.abs(k - r) < 0.05) ok++; }
      return ok / iv.length;
    });
    const best = Math.max(...scores);
    if (best < 0.7) return 0;
    for (let i = 0; i < STANDARD_BPH.length; i++) if (scores[i] >= best - 0.05) return STANDARD_BPH[i];
    return 0;
  }

  // ------------------------------------------------------------ amplitude
  // The tick contains an unlock burst (A) and a drop/lock burst (C). The time
  // between them is how long the balance takes to sweep the lift angle L:
  //   dt = asin(L / 2A) / (pi f)   =>   A = L / (2 sin(pi f dt)),  f = bph / 7200
  function measureAmplitude(env, pre, fs, bph, lift) {
    const f = bph / 7200;
    const tmin = Math.asin(lift / 720) / (Math.PI * f);  // A = 360 deg
    const tmax = Math.asin(lift / 260) / (Math.PI * f);  // A = 130 deg
    let max = 0;
    for (const v of env) if (v > max) max = v;
    const w = Math.max(1, Math.round(fs * 0.00025));
    const peaks = [];
    for (let i = Math.max(1, pre - Math.round(fs * 0.0005)); i < env.length - 1; i++) {
      const v = env[i];
      if (v < 0.2 * max || v <= env[i - 1]) continue;
      let isPeak = true;
      for (let j = Math.max(0, i - w); j <= Math.min(env.length - 1, i + w); j++) if (env[j] > v) { isPeak = false; break; }
      if (isPeak && (!peaks.length || i - peaks[peaks.length - 1] > w)) peaks.push(i);
    }
    if (peaks.length < 2) return null;
    // burst start: walk back from the peak to a valley or to 30% of the peak
    const startOf = p => {
      let j = p;
      const lim = 0.3 * env[p];
      while (j > 0 && env[j - 1] <= env[j] && env[j - 1] > lim) j--;
      return j;
    };
    const a = startOf(peaks[0]);
    let c = -1, cPeak = -1;
    for (let k = 1; k < peaks.length; k++) {
      const s = startOf(peaks[k]), dt = (s - a) / fs;
      if (dt >= tmin * 0.9 && dt <= tmax && env[peaks[k]] >= (cPeak < 0 ? 0 : env[cPeak])) { c = s; cPeak = peaks[k]; }
    }
    if (c < 0) return null;
    const dt = (c - a) / fs;
    const amp = lift / (2 * Math.sin(Math.PI * f * dt));
    if (!(amp > 100 && amp < 380)) return null;
    return { amp, aIdx: a, cIdx: c, dt };
  }

  // Samples from the trigger to the start of the strongest burst in a snapshot.
  function strongestBurstOffset(env, pre) {
    let pk = 0;
    for (let i = 1; i < env.length; i++) if (env[i] > env[pk]) pk = i;
    let j = pk;
    const lim = 0.3 * env[pk];
    while (j > 0 && env[j - 1] <= env[j] && env[j - 1] > lim) j--;
    return j - pre;
  }

  // ------------------------------------------------------------ least squares
  // Fit t = a + b*n + c*parity(n). b is the measured beat period; c is the
  // offset of odd beats from the even-beat grid, which is the beat error.
  function fitBeats(pts) {
    const n0 = pts[0].n, t0 = pts[0].t;
    const M = [[0, 0, 0], [0, 0, 0], [0, 0, 0]], v = [0, 0, 0];
    for (const p of pts) {
      const x = [1, p.n - n0, p.p], y = p.t - t0;
      for (let i = 0; i < 3; i++) { v[i] += x[i] * y; for (let j = 0; j < 3; j++) M[i][j] += x[i] * x[j]; }
    }
    const s = solve3(M, v);
    if (!s) return null;
    const res = pts.map(p => p.t - t0 - (s[0] + s[1] * (p.n - n0) + s[2] * p.p));
    return { a: s[0], b: s[1], c: s[2], res, t0, n0 };
  }

  function solve3(M, v) {
    const A = M.map((r, i) => [...r, v[i]]);
    for (let i = 0; i < 3; i++) {
      let piv = i;
      for (let r = i + 1; r < 3; r++) if (Math.abs(A[r][i]) > Math.abs(A[piv][i])) piv = r;
      if (Math.abs(A[piv][i]) < 1e-12) return null;
      [A[i], A[piv]] = [A[piv], A[i]];
      for (let r = 0; r < 3; r++) {
        if (r === i) continue;
        const k = A[r][i] / A[i][i];
        for (let c = i; c < 4; c++) A[r][c] -= k * A[i][c];
      }
    }
    return [A[0][3] / A[0][0], A[1][3] / A[1][1], A[2][3] / A[2][2]];
  }

  // ------------------------------------------------------------ Analyzer
  class Analyzer {
    constructor(opts = {}) {
      this.opts = { liftAngle: 52, bph: 0, ppm: 0, windowSec: 20, fs: 48000 };
      Object.assign(this.opts, opts);
      this.reset();
    }
    reset() {
      this.recent = [];        // recent tick times for BPH detection
      this.bph = this.opts.bph || 0;
      this.run = [];           // accepted ticks of the current run: {t, n, p, amp}
      this.trace = [];         // {row, x (s, residual vs nominal grid), p}
      this.row = 0;
      this.rejects = 0;
      this.acGaps = [];        // trigger -> strongest burst gaps of clean ticks (s)
      this.result = null;
      this.history = [];       // {t, rate, be, amp}
      this.lastTick = null;
      this.ticksSeen = 0;
    }
    setOptions(o) {
      const restart = ('bph' in o && o.bph !== this.opts.bph) || ('ppm' in o && o.ppm !== this.opts.ppm);
      Object.assign(this.opts, o);
      if (restart) this.reset();
      else if (this.run.length) this.result = this.compute();
    }
    get period() { return this.bph ? 3600 / this.bph : 0; }
    newRun() { this.run = []; this.row += 3; this.rejects = 0; }

    // tick: firmware/detector tick object. Returns the tick annotated with
    // {t, n, parity, amp, rejected} for display.
    addTick(tick) {
      const fs = this.opts.fs * (1 + this.opts.ppm * 1e-6);
      const tTrig = tick.onset / fs;
      this.ticksSeen++;
      this.recent.push(tTrig);
      if (this.recent.length > 41) this.recent.shift();

      if (!this.opts.bph && (this.recent.length >= 9) && (!this.bph || this.ticksSeen % 20 === 0)) {
        const iv = [];
        for (let i = 1; i < this.recent.length; i++) iv.push(this.recent[i] - this.recent[i - 1]);
        const b = detectBph(iv);
        if (b && b !== this.bph) { this.bph = b; this.newRun(); this.trace = []; this.history = []; }
      }
      const info = { ...tick, t: tTrig, n: null, parity: null, amp: null, rejected: false, late: false };
      this.lastTick = info;
      if (!this.bph) return info;
      const T = this.period;

      // Timing reference is the unlock (A), the first sound of the tick. A quiet
      // A doesn't always cross the threshold, so the trigger can land on a later
      // burst (impulse B or drop C). The strongest burst (normally C) is caught
      // every time, so learn the usual trigger-to-strongest gap from ticks that
      // did trigger on A, and when a tick's gap is shorter than that, the trigger
      // was late: place it at (strongest burst - usual gap) instead.
      const so = strongestBurstOffset(tick.env, tick.pre) / this.opts.fs;
      const gap = this.acGaps.length >= 5 ? median(this.acGaps) : null;
      if (so > 0.003 && (gap == null || Math.abs(so - gap) < 0.0015)) {
        this.acGaps.push(so);
        if (this.acGaps.length > 60) this.acGaps.shift();
      }
      const cands = [tTrig];
      if (gap != null && gap - so > 0.0015) cands.unshift(tTrig - (gap - so) / (1 + this.opts.ppm * 1e-6));

      let n, t = cands[0];
      if (!this.run.length) n = 0;
      else {
        const last = this.run[this.run.length - 1];
        const f = this.result;
        const locked = f && f.count >= 20 && f.jitterMs < 2;
        let ok = false;
        for (const c of cands) {
          if (locked) {
            // predict this beat from the fit (period, phase, tick/tock offset)
            n = f.n0 + Math.round((c - f.t0 - f.a - f.c / 2) / f.b);
            const pred = f.t0 + f.a + f.b * (n - f.n0) + f.c * (n & 1);
            const tol = Math.min(0.004, Math.max(0.002, 6 * f.jitterMs / 1000));
            ok = n > last.n && Math.abs(c - pred) <= tol;
          } else {
            const r = Math.round((c - last.t) / T);
            n = last.n + r;
            ok = r >= 1 && r <= 6 && Math.abs(c - last.t - r * T) <= Math.max(0.006, 0.04 * T);
          }
          if (ok) { t = c; break; }
        }
        if (!ok) {
          info.rejected = true;
          this.rejects++;
          // a run that never got going was probably seeded by a stray click;
          // one with nothing usable for a while has lost the watch. Start over.
          if (this.run.length < 4 || tTrig - last.t > 6 * T) { this.newRun(); n = 0; t = cands[0]; info.rejected = false; }
          else return info;
        }
      }
      info.t = t;
      info.late = t !== tTrig;
      // amplitude needs the A burst in the snapshot, which late triggers lack
      if (!info.late) {
        const am = measureAmplitude(tick.env, tick.pre, this.opts.fs, this.bph, this.opts.liftAngle);
        if (am) Object.assign(info, { amp: am.amp, aIdx: am.aIdx, cIdx: am.cIdx });
      }
      this.rejects = 0;
      const p = n & 1;
      if (!this.run.length) { this.t0 = t; this.rowBase = this.row; }
      this.run.push({ t, n, p, amp: info.amp });
      info.n = n; info.parity = p;
      this.row = this.rowBase + n;
      this.trace.push({ row: this.row, x: t - (this.t0 + n * T), p });
      if (this.trace.length > 4000) this.trace.splice(0, this.trace.length - 4000);

      // drop ticks older than the window (keep the whole run for long-term)
      const tMin = t - this.opts.windowSec;
      let cut = 0;
      while (cut < this.run.length && this.run[cut].t < tMin) cut++;
      if (cut) this.run.splice(0, cut);

      this.result = this.compute();
      // A run that has plenty of beats but still doesn't fit a clean rhythm
      // has mixed in stray clicks (often seeded by one after a disturbance).
      if (this.result && this.result.count >= 12 && this.result.jitterMs > 2) { this.newRun(); this.result = null; }
      if (this.result) this.history.push({ t, rate: this.result.rate, be: this.result.beatError, amp: this.result.amp });
      if (this.history.length > 5000) this.history.shift();
      return info;
    }

    compute() {
      const T = this.period;
      let pts = this.run;
      if (pts.length < 8 || !pts.some(x => x.p) || !pts.some(x => !x.p)) return null;
      let fit = fitBeats(pts);
      if (!fit) return null;
      // one round of outlier rejection
      const mad = median(fit.res.map(Math.abs)) || 1e-6;
      const keep = pts.filter((_, i) => Math.abs(fit.res[i]) <= Math.max(6 * mad, 50e-6));
      if (keep.length >= 8 && keep.length < pts.length) { const f2 = fitBeats(keep); if (f2) { fit = f2; pts = keep; } }
      const rate = (T - fit.b) / T * 86400;
      const sd = Math.sqrt(fit.res.reduce((s, r) => s + r * r, 0) / Math.max(1, fit.res.length - 3));
      const amps = pts.map(x => x.amp).filter(x => x != null);
      const ampEven = pts.filter(x => !x.p && x.amp != null).map(x => x.amp);
      const ampOdd = pts.filter(x => x.p && x.amp != null).map(x => x.amp);
      return {
        bph: this.bph, period: fit.b, rate,
        t0: fit.t0, n0: fit.n0, a: fit.a, b: fit.b, c: fit.c,
        beatError: Math.abs(fit.c) * 1000,
        amp: amps.length >= 3 ? median(amps) : null,
        ampEven: ampEven.length ? median(ampEven) : null,
        ampOdd: ampOdd.length ? median(ampOdd) : null,
        jitterMs: sd * 1000, count: pts.length,
        span: pts[pts.length - 1].t - pts[0].t,
      };
    }
  }

  // ------------------------------------------------------------ synthetic watch
  // Generates microphone-like audio of a mechanical watch for the simulator
  // and tests. Each tick = unlock, impulse and drop bursts (ringing decays).
  class SyntheticWatch {
    constructor(o = {}) {
      this.o = Object.assign({
        fs: 48000, bph: 28800, rate: 5, beatErrorMs: 0.6, amplitude: 275, lift: 52,
        level: 0.05, noise: 0.0004, hum: 0.02, jitterUs: 15, missProb: 0, clockPpm: 0,
        quietAProb: 0, strayPerSec: 0,   // unlock too quiet to trigger; random loud clicks between beats
      }, o);
      this.idx = 0; this.beat = 0; this.events = [];
      this.seed = 12345;
      this.scheduleNext();
    }
    rnd() { this.seed = (this.seed * 1664525 + 1013904223) >>> 0; return this.seed / 4294967296; }
    gauss() { const u = Math.max(1e-12, this.rnd()), v = this.rnd(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
    set(o) { Object.assign(this.o, o); }
    scheduleNext() {
      const o = this.o, T = 3600 / o.bph * (1 - o.rate / 86400);
      const f = o.bph / 7200, dt = Math.asin(o.lift / (2 * o.amplitude)) / (Math.PI * f);
      // the ADC clock may itself be off by clockPpm: sample index = true time * fs * (1+ppm)
      const fsEff = o.fs * (1 + o.clockPpm * 1e-6);
      const tt = 0.3 + this.beat * T + (this.beat & 1 ? o.beatErrorMs / 1000 : 0) + this.gauss() * o.jitterUs * 1e-6;
      this.beat++;
      if (this.rnd() < o.missProb) { this.nextAt = (tt + T * 0.5) * fsEff; this.pendingBursts = []; return; }
      this.nextAt = tt * fsEff;
      this.pendingBursts = [
        { at: tt, a: this.rnd() < o.quietAProb ? 0.01 : 0.35, fr: 4500 + 1500 * this.rnd(), tau: 0.0004 },
        { at: tt + dt * 0.45, a: 0.55, fr: 3500 + 1500 * this.rnd(), tau: 0.0005 },
        { at: tt + dt, a: 1.0, fr: 5000 + 2000 * this.rnd(), tau: 0.0007 },
      ].map(b => ({ ...b, at: b.at * fsEff, tau: b.tau * fsEff, w: 2 * Math.PI * b.fr / fsEff }));
    }
    render(n) {
      const o = this.o, out = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        const s = this.idx + i;
        if (s >= this.nextAt - 1) { for (const b of this.pendingBursts) this.events.push(b); this.scheduleNext(); }
        if (o.strayPerSec && this.rnd() < o.strayPerSec / o.fs)
          this.events.push({ at: s, a: 0.3 + 0.7 * this.rnd(), w: 2 * Math.PI * (3000 + 4000 * this.rnd()) / o.fs, tau: 0.0006 * o.fs });
        let x = this.gauss() * o.noise + o.hum * Math.sin(2 * Math.PI * 50 * s / o.fs);
        for (const b of this.events) {
          const d = s - b.at;
          if (d >= 0) x += o.level * b.a * Math.exp(-d / b.tau) * Math.sin(b.w * d);
        }
        out[i] = x;
      }
      this.idx += n;
      this.events = this.events.filter(b => this.idx - b.at < b.tau * 12);
      return out;
    }
  }

  const api = { STANDARD_BPH, median, Detector, FrameParser, decodeTick, detectBph, measureAmplitude, fitBeats, Analyzer, SyntheticWatch };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TG = api;
})(typeof window !== 'undefined' ? window : globalThis);
