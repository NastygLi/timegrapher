# Hardware

## Parts

| Part | Notes |
|---|---|
| Raspberry Pi Pico (RP2040) | The plain Pico is enough; no WiFi needed. A Pico W, Pico 2 or Pico 2 W also works. |
| INMP441 I2S MEMS microphone module | The common 6-pin breakout (VDD, GND, L/R, SCK, WS, SD) |
| Micro-USB data cable | Must carry data, not just power |
| 6 short wires, or headers + jumpers | Keep them under about 10 cm |

You'll also want a soldering iron, because the INMP441 board usually ships
with its header pins loose. Jumper wires pushed through unsoldered holes make
intermittent contact, which shows up as dropouts and noise.

## Wiring

| INMP441 | Pico GPIO | Pico physical pin |
|---|---|---|
| VDD | 3V3(OUT) | 36 |
| GND | GND | 3 (or any GND) |
| L/R | GND | any GND |
| SCK | GP2 | 4 |
| WS  | GP3 | 5 |
| SD  | GP4 | 6 |

Physical pins are numbered from pin 1 at the top-left with the USB socket at
the top, running down the left edge. Pins 3–6 sit together near the USB end.

```
            USB
        ┌───┴┴┴───┐
 GP0  1 │●        │ 40 VBUS
 GP1  2 │●        │ 39 VSYS
 GND  3 │●  ◀ GND │ 38 GND
 GP2  4 │●  ◀ SCK │ 37 3V3_EN
 GP3  5 │●  ◀ WS  │ 36 3V3(OUT)  ◀ VDD
 GP4  6 │●  ◀ SD  │ ...
        │  Pico   │
```

Notes:

- **WS must be the GPIO directly after SCK.** The PIO I2S program generates both
  clocks from consecutive pins. To move the mic, change `PIN_SCK` and `PIN_SD`
  at the top of `firmware/pico_timegrapher/pico_timegrapher.ino`, and wire WS to
  `PIN_SCK + 1`.
- **L/R** selects which I2S slot the mic uses. Tie it to GND; don't leave it
  floating. The firmware listens to both slots and picks the one with signal, so
  either strap works.
- Power the mic from **3V3(OUT)**, not VBUS (5 V). The INMP441 is a 3.3 V part.

## Mounting the microphone

This matters more than anything else for good readings. The INMP441 hears
through the air via a small port hole, on the opposite side of the board from
the chip on most modules. A ticking watch is quiet, so:

- **Get the case close to the port.** Lay the watch on the mic or press the
  caseback against it. Mechanical contact transmits the tick far better than an
  air gap does.
- **Isolate it from the table.** A piece of foam under the mic board stops desk
  vibrations and keyboard taps from triggering ticks.
- **Hold the watch still.** A simple cradle or clamp helps a lot: a 3D-printed
  holder, a clothes peg, or a rubber band around the watch and a foam block.
  The same contact in every position gives consistent positional readings.
- **Quiet room.** The detector adapts to background noise, but fans, HVAC and
  speech all reduce the signal-to-noise ratio (SNR).

The page header shows peak level, noise floor and SNR. Aim for **20 dB or more**.
The Pico's LED flashes on every detected tick, which is useful while you find
the best spot.

## Flashing the firmware

### Option A: copy the UF2 (no tools needed)

1. Build the UF2 (see below), or grab one from a release if available.
2. Hold the **BOOTSEL** button on the Pico while plugging it in. A drive named
   `RPI-RP2` appears.
3. Copy `pico_timegrapher.ino.uf2` onto it. The Pico reboots into the timegrapher.

### Option B: build with arduino-cli

The firmware uses the [arduino-pico](https://github.com/earlephilhower/arduino-pico)
core and the ArduinoJson library.

```sh
# one-time setup
arduino-cli core install rp2040:rp2040 \
  --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli lib install ArduinoJson

# build
arduino-cli compile --fqbn rp2040:rp2040:rpipico firmware/pico_timegrapher --output-dir build

# first flash: BOOTSEL + plug in, then
cp build/pico_timegrapher.ino.uf2 /Volumes/RPI-RP2/          # macOS; use the drive letter on Windows

# later updates (no BOOTSEL needed while the timegrapher firmware is running)
arduino-cli upload -p /dev/cu.usbmodem101 --fqbn rp2040:rp2040:rpipico firmware/pico_timegrapher
```

The Arduino IDE also works: add the board-manager URL above, select
*Raspberry Pi Pico*, install ArduinoJson, open the sketch and click Upload.

If the serial port doesn't appear after the first flash, unplug the Pico and
plug it back in.

### Status LED

| LED | Meaning |
|---|---|
| Slow blink (once a second) | Running, no page connected |
| Solid on | Page connected |
| Quick flash on every tick | Hearing the watch |
