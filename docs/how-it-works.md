# How it works

The system is split so that the microcontroller does what needs precise timing,
and the browser does what benefits from being easy to change.

```
INMP441 ─I2S─▶ [Pico core 1]                                   [browser]
               24-bit samples @ 48 kHz                         frame parser
               4th-order high-pass (2 kHz)                     BPH detection
               peak-hold envelope (~0.5 ms release)            beat numbering
               adaptive noise floor                            least-squares fit → rate, beat error
               threshold crossing → onset (sub-sample)         A/C bursts → amplitude
               40 ms envelope snapshot                         trace, plots
               [Pico core 0] ──USB serial frames──────────────▶
```

## On the Pico

### Sampling

The INMP441 is a digital microphone: it outputs 24-bit samples over I2S, so
there's no analogue stage to tune. The Pico generates the I2S clocks with a PIO
state machine at 48 kHz and reads both stereo slots. The mic uses one slot,
selected by its L/R pin, and the firmware picks whichever slot carries signal.

**Every timestamp in the system is a sample index.** Sample *n* happened at
time *n / fs*, where fs is set by the Pico's crystal. Software timers would add
jitter; the sample clock doesn't.

The PIO clock divider has only 8 fractional bits, so the real sample rate is
slightly off 48,000 Hz. On a stock Pico at 200 MHz it is 48,001.92 Hz, which is
+40 ppm or about 3.5 s/day. At startup the firmware reads the divider back from
the hardware, computes the exact rate, and reports it to the browser, which
uses it for all timing.

### Detecting ticks

The RP2040 has no hardware floating point, so the per-sample processing is all
integer maths (fixed point):

1. **High-pass filter.** Two cascaded biquads make a 4th-order Butterworth
   high-pass at 2 kHz by default, with Q29 coefficients and 64-bit
   accumulators. Watch ticks are rich in 2–10 kHz content; this removes hum,
   handling noise and most room rumble.
2. **Envelope.** This is a peak-hold of the absolute value, decaying by
   `1 − 1/32 − 1/128` per sample (about 0.5 ms). It follows each burst in the
   tick but smooths out the individual oscillations.
3. **Noise floor.** This asymmetric tracker falls quickly (about 21 ms) and rises
   slowly (about 2.7 s), so it settles to the quiet level *between* ticks and
   ignores the ticks themselves.
4. **Trigger.** A tick starts when the envelope crosses
   `max(floor × sensitivity, minimum)`. The crossing time is interpolated
   linearly between samples, giving sub-sample (sub-21 µs) resolution. After a
   trigger, new triggers are ignored for a refractory period. The browser sets
   this to 60% of the beat period once BPH is known, so the later bursts of the
   same tick don't count as new ticks.
5. **Snapshot.** The envelope is kept in a ring buffer. 38 ms after a trigger,
   the window from 2 ms before to 38 ms after is scaled to 16 bits and queued
   with the onset time.

Core 1 runs all of this. Core 0 only moves queued ticks out over USB, so a slow
or absent host never disturbs sampling.

## In the browser

### Beat rate

Watches beat at standard rates. For each candidate (12,000, 14,400, 18,000,
19,800, 21,600, 25,200, 28,800, 36,000, 43,200 bph), the analyzer scores the
share of recent tick intervals that fall on a whole number of beats, within
5%. Missed ticks still fit, since 2 or 3 beats are allowed. The highest score
wins; ties go to the slowest rate, because every interval that fits a slow rate
also fits its multiples.

### Numbering beats

Each tick gets a beat number *n*, by rounding the elapsed time since the
previous tick to a whole number of beat periods. A tick more than 20% of a
period away from any whole beat is rejected as noise. Four rejections in a row
restart the sequence, for example after the watch is moved.

### Rate and beat error

For the ticks in the averaging window, the analyzer fits by least squares

```
t(n) = a + b·n + c·parity(n)
```

where parity is 0 for even beats (tick) and 1 for odd (tock).

- **b** is the measured beat period. Rate = `(T − b) / T × 86400` s/day, where T
  is the nominal period `3600 / bph`.
- **c** is how far tocks sit from the grid that the ticks define. The two
  intervals alternate between `b + c` and `b − c`, so **beat error = |c|**.

The fit is repeated once with outliers (more than 6× the median residual)
removed. σ in the UI is the residual standard deviation, a direct measure of
timing noise.

This uses every tick in the window at once, so it's much more precise than
averaging individual intervals. On synthetic data with 15 µs of timing jitter,
rate comes out within ±0.05 s/day over 30 seconds.

### Amplitude

During each beat the balance wheel passes through its centre, where the pallet
fork unlocks the escape wheel (sound **A**), receives impulse, and drops/locks
(sound **C**). The fork is engaged only while the balance roller jewel sweeps
the **lift angle** L. The balance moves as `θ(t) = A·sin(2π·f·t)`, where
f = bph / 7200 is the oscillation frequency, so the time to sweep ±L/2 around
the centre is

```
Δt = asin(L / 2A) / (π·f)      ⇒      A = L / (2·sin(π·f·Δt))
```

From the tick snapshot, the analyzer:
1. finds peaks above 20% of the maximum;
2. takes the start of the first burst as **A**;
3. takes as **C** the strongest later burst whose spacing from A corresponds
   to an amplitude between 130° and 360°;
4. measures Δt between the burst *starts* and converts it to degrees.

The displayed value is the median over the window, also split by tick and tock.
Amplitude is the reading most sensitive to the microphone: which bursts are
audible, and how sharply they start, depend on the watch and the mounting. The
*Last tick* plot shows exactly what was measured.

## Accuracy budget

| Source | Effect | Handled by |
|---|---|---|
| PIO fractional divider | ~40 ppm (~3.5 s/d) | Measured from hardware; corrected automatically |
| Crystal tolerance | ±10–30 ppm (±1–2.5 s/d) | *Clock correction* setting |
| Timing jitter (noise, onset detection) | Adds scatter, averaged out by the fit | Least-squares over the window |
| Threshold-crossing bias (louder ticks cross earlier) | Constant offset; cancels in rate; small effect on beat error if tick and tock differ a lot in loudness | — |
| Amplitude | Depends on burst identification | Visual check in *Last tick*; correct lift angle |

## Testing

`test/analysis.test.js` synthesizes realistic watch audio: three decaying
bursts per tick at the positions the amplitude formula predicts, plus noise,
50 Hz hum, timing jitter, missed ticks and clock error. It runs the audio
through the same detector and analyzer the UI uses. Run it with
`node test/analysis.test.js`.
