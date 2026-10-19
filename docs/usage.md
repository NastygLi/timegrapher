# Using the timegrapher

## Taking a reading

1. Plug in the Pico and open `web/index.html` in **Chrome or Edge**. Safari and
   Firefox don't support Web Serial, which the page needs to talk to the Pico.
2. Click **Connect Pico** and choose the Pico from the list. The status changes
   to *Pico · live* and the Pico's LED stays on. Next time, the page reconnects
   by itself.
3. Wind the watch fully and let it run a minute.
4. Put it on the microphone (see [mounting](hardware.md#mounting-the-microphone)).
   In the header, the level bar should jump past the threshold line on every
   tick.
5. After a few seconds, **Beat rate** is detected and the trace starts drawing.
   Give it 20–30 seconds for rate and beat error to settle.

## Reading the display

### Readouts

| Readout | Meaning | Typical good values |
|---|---|---|
| **Rate** (s/d) | Seconds gained (+) or lost (−) per day at the current rate | Within ±10 s/d for a regulated watch; COSC chronometers are −4/+6 |
| **Amplitude** (°) | How far the balance swings from rest, one way | 250–310° fully wound, horizontal; below about 200° often means it needs service |
| **Beat error** (ms) | Difference in timing between tick and tock | Below 0.6 ms is good; above about 1.5 ms needs adjustment |
| **Beat rate** (bph) | Beats per hour of the movement | 28,800 is common in modern watches; 21,600 and 18,000 in older ones |

The small text under each readout shows extra detail:
- **σ:** timing scatter, which is lower when the signal is clean.
- **Beats:** how many beats are in the averaging window.
- **Tick/tock amplitudes:** these should be close to each other.

### Trace

This is the classic timegrapher paper strip. Each dot is one beat, plotted
against a perfect clock: tick in blue, tock in pink. New beats appear at the bottom.

- **Vertical lines** mean the rate is perfect.
- **Lines leaning right** mean the watch is fast; **leaning left** means slow.
  The steeper the lean, the bigger the error.
- **Two separate lines** mean beat error; the gap between them is the beat
  error. One merged line means zero beat error.
- **Scattered dots** mean noise or a weak signal. Improve mic contact, or raise
  sensitivity.
- **Wavy lines** can show a real issue, such as a poised balance or escape
  wheel problem, but check the mounting first.

Lines that drift off one edge reappear on the other. Change **Trace width** to
zoom.

### Last tick

This is the sound envelope of the most recent tick. A healthy Swiss lever
escapement tick has several bursts: the **unlock (A)**, the **impulse**, and the
**drop (C)**. The blue A and pink C markers show where the amplitude
measurement placed them. If the markers sit on noise or on the wrong bursts,
the amplitude reading is unreliable; improve the mic coupling.

### Rate history

This shows rate over the last 10 minutes. It's useful for seeing the watch settle,
or for comparing before and after an adjustment.

## Settings

| Setting | What it does |
|---|---|
| **Beat rate** | *Auto* detects it; pick a value if detection is wrong. Auto can lock onto half the real rate when every second tick is missed. |
| **Lift angle** | Property of the movement, used only for amplitude. Most modern movements are 50–53° (ETA 2824: 50°, many Seiko: 53°, Miyota 8215: 50°, older ETA/Unitas: 52°). Look up your calibre. A wrong lift angle scales amplitude proportionally. |
| **Averaging window** | Seconds of beats used for rate and beat error. A longer window is steadier but slower to react. |
| **Trace width** | Horizontal span of the trace. |
| **Sensitivity** | Trigger threshold as a multiple of the noise floor. Lower it for quiet watches; raise it if room noise causes false triggers. |
| **High-pass filter** | Cuts low-frequency noise (hum, handling, desk thumps). The default is 2 kHz; try 1 kHz if ticks are dull or weak. |
| **Clock correction** | Compensates for the Pico crystal's error; see below. |
| **Reset measurement** | Starts fresh, for example after moving the watch. |

Settings are remembered between visits.

## Positional readings

Watches run differently depending on how they're held. To check positions:

1. Pick a position (*Dial up*, *Dial down*, *Crown up*, …).
2. Let the reading settle for about 30 seconds, then click **Save reading**. The
   selector moves to the next position automatically.
3. Repeat. The table shows the **average** rate and the **Δ max** (the spread
   between the best and worst position). A lower Δ means a better-adjusted watch.

Readings are stored in the browser; **Clear** removes them.

## Calibrating the clock

Every rate measurement is relative to the Pico's 12 MHz crystal. Crystals are
typically within ±10–30 ppm, and **1 ppm = 0.0864 s/day** of rate error, so an
uncalibrated Pico can be off by a couple of seconds a day. Beat error and
amplitude aren't affected.

To calibrate, measure something whose true rate you know:

- A watch you've had checked on a professional timegrapher, or
- A watch you've tracked against an internet time source over several days.

If the timegrapher reads it **X s/d higher** than its true rate, enter
**−X × 11.57** in *Clock correction (ppm)*. For example, a watch that is
really +2 s/d but reads +3 s/d gives X = 1, so enter −11.6. The correction is
saved and applies to every future reading on this Pico.

The *sample rate* error caused by the Pico's fractional clock divider is a
separate effect, which the firmware corrects automatically. Clock correction
is only for the crystal.

## Troubleshooting

| Symptom | Things to try |
|---|---|
| No **Connect Pico** button | Use Chrome or Edge. |
| Pico not in the port list | Use a data cable (not charge-only); replug; check that the LED blinks. |
| LED doesn't blink at all | Firmware isn't running; reflash (see [hardware](hardware.md#flashing-the-firmware)). |
| Connected, but the level meter never moves | Check the wiring, especially SD, SCK and WS, and the 3V3 supply; make sure L/R is tied to GND. |
| Level moves but no ticks are detected | Improve contact with the mic; lower **Sensitivity**; try a 1 kHz high-pass. |
| Scattered dots and jumpy readings | Room or desk noise. Use foam under the mic, a quieter spot, and higher **Sensitivity**. |
| Beat rate detected as half the real value | Every other tick is being missed. Improve the signal, or set the beat rate by hand. |
| Amplitude missing or implausible | Check the *Last tick* markers and the lift angle. Amplitude needs a clean, close signal. |
| Rate consistently off versus a known reference | Calibrate the clock (above). |
