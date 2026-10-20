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

There are two views, chosen with **Trace view**.

**Dial** (default) works like the *Watch Accuracy Meter* app. On a cream "paper" background, the
trace grows up from a pivot at the bottom, newest beat at the pivot, and acts like a needle
pointing at the rate on the arc scale:

- **Straight up** means the watch is on time. **Leaning right** means it's gaining; **leaning left**
  means it's losing. Read the rate where the line meets the arc.
- **The zig-zag width** shows beat error (tick and tock land on two slightly different lines).
- The black **pointer** on the arc marks the measured rate. The **Δ bracket** under it shows how
  much the rate varied over the averaging window; a small Δ means a steady reading.
- **Dial range** sets the scale at the edges of the fan: ±10, ±25, ±50 or ±100 s/d.

**Strip** is a scrolling chart:

Each dot is one beat: tick in
blue, tock in pink. The newest beat is on the right, and the last minute or so is shown. Height
shows how early or late each beat came compared with a perfect clock, in milliseconds.

- **A level line** means the watch is on time.
- **A rising line** means it's gaining (fast); **a falling line** means it's losing (slow).
  The steeper the slope, the bigger the error.
- **Two parallel lines** mean beat error; the vertical gap between them is the beat error.
  One merged line means zero beat error.
- **Scattered dots** mean noise or a weak signal. Improve the mic contact.
- **Wavy lines** can show a real issue. A wave that repeats about once a minute, with
  amplitude rising and falling in step, points at the seconds (fourth) wheel: wear, dirt or
  a bent pivot. Faster waves can point at the escape wheel. Check the mounting first.

The view stays centred on the visible dots. **Trace scale** set to *Auto* zooms to fit
them; a fixed scale wraps dots that run off the top or bottom, like timegrapher paper.

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
| **Calibre** | Picks the lift angle for common movements. Choose *Custom* to type one. |
| **Lift angle** | Property of the movement, used only for amplitude. No timegrapher can measure it from sound: amplitude and lift angle are tied together in one equation, so bench machines look it up by calibre too. Most modern movements are 50–53°. A wrong lift angle scales amplitude proportionally. |
| **Averaging window** | Seconds of beats used for rate and beat error. The default, 60 s, covers one full turn of the seconds wheel, whose small irregularities otherwise make a short-window rate swing up and down. Use 10–20 s while adjusting the regulator, so the reading reacts faster. |
| **Trace view** | *Dial* (like Watch Accuracy Meter) or *Strip* (scrolling chart). |
| **Dial range** | Rate at the edges of the dial's fan. |
| **Trace scale** | Vertical range of the strip view. *Auto* fits the visible dots. |
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
