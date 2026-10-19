// Raspberry Pi Pico (RP2040) + INMP441 Timegrapher
//
// Core 1 samples the INMP441 at 48 kHz over PIO I2S and runs the tick
// detector in fixed point (the RP2040 has no FPU). Each tick is timestamped
// in samples and queued with a 40 ms envelope snapshot. Core 0 streams the
// queue to the host over USB serial; the browser UI (web/index.html, Chrome
// or Edge via Web Serial) does the rate / beat error / amplitude maths.
//
// Wiring:
//   INMP441 VDD -> 3V3(OUT) pin 36   INMP441 GND -> GND pin 3
//   INMP441 L/R -> GND               (firmware auto-detects either slot)
//   INMP441 SCK -> GP2 (pin 4)       INMP441 WS  -> GP3 (pin 5, must be SCK+1)
//   INMP441 SD  -> GP4 (pin 6)
//
// Serial framing (Pico -> host): A5 5A, u8 type, u16 len, payload, u8 sum(payload)
//   type 1: tick (same layout as the ESP32 WebSocket frame)
//   type 2: stats JSON
// Host -> Pico: one JSON config object per line.

#include <I2S.h>
#include <ArduinoJson.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>

// ---------------------------------------------------------------- config
#define PIN_SCK 2   // WS is always PIN_SCK + 1
#define PIN_SD  4

static const int FS = 48000;
static const int PRE  = FS * 2 / 1000;    // envelope snapshot: 2 ms before onset
static const int POST = FS * 38 / 1000;   //                    38 ms after
static const int SNAP = PRE + POST;
static const int RING = 4096;             // power of two, > SNAP
static const int SLOTS = 4;               // tick queue core 1 -> core 0
static const float FULL_SCALE = 8388608.0f;   // 2^23: 24-bit samples

// ---------------------------------------------------------------- types
// (declared before any function so Arduino's generated prototypes can see them)

// Fixed-point biquad: Q29 coefficients, 24-bit samples, 64-bit accumulator.
struct BiquadQ {
  int32_t b0 = 1 << 29, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  void highpass(float fc, float q) {
    float w0 = 2.0f * (float)M_PI * fc / FS;
    float c = cosf(w0), alpha = sinf(w0) / (2.0f * q), a0 = 1.0f + alpha;
    const float S = 536870912.0f;   // 2^29
    b0 = lroundf((1.0f + c) / 2.0f / a0 * S);
    b1 = lroundf(-(1.0f + c) / a0 * S);
    b2 = b0;
    a1 = lroundf(-2.0f * c / a0 * S);
    a2 = lroundf((1.0f - alpha) / a0 * S);
    x1 = x2 = y1 = y2 = 0;
  }
  inline int32_t run(int32_t x) {
    int64_t acc = (int64_t)b0 * x + (int64_t)b1 * x1 + (int64_t)b2 * x2
                - (int64_t)a1 * y1 - (int64_t)a2 * y2;
    int32_t y = (int32_t)(acc >> 29);
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  }
};

struct TickSlot {
  double onset;            // samples since boot, interpolated
  float peak, floor, thr;  // full scale = 1
  uint16_t env[SNAP];      // envelope, peak = 65535
};

// ---------------------------------------------------------------- shared state
static volatile float cfgHpfHz = 2000.0f;
static volatile float cfgSens = 6.0f;
static volatile float cfgMinThr = 0.0002f;
static volatile float cfgRefractoryMs = 40.0f;
static volatile bool  cfgDirty = true;

static volatile double fsActual = FS;     // from the PIO clock divider
static volatile uint64_t stSamples = 0;
static volatile uint32_t stTicks = 0, stDropped = 0, stOverruns = 0;
static volatile int32_t stFloor = 0, stThr = 0, stPeak = 0;
static volatile int stChannel = -1;
static volatile bool core1Ready = false;

static TickSlot slots[SLOTS];
static volatile uint32_t slotHead = 0, slotTail = 0;   // core 1 writes head, core 0 writes tail

static I2S i2s(INPUT);

// ---------------------------------------------------------------- helpers
// The I2S library programs a fractional PIO clock divider, so the real sample
// rate is slightly off 48 kHz. Find the running I2S state machine and compute
// the exact rate from its divider (PIO runs 2 cycles per bit, 64 bits/frame).
static double measureI2sRate() {
  PIO pios[] = { pio0, pio1 };
  double best = FS;
  for (PIO p : pios) {
    for (int sm = 0; sm < 4; sm++) {
      if (!(p->ctrl & (1u << (PIO_CTRL_SM_ENABLE_LSB + sm)))) continue;
      uint32_t cd = p->sm[sm].clkdiv;
      double div = (cd >> 16) + ((cd >> 8) & 0xff) / 256.0;
      if (div == 0) div = 65536;
      double fs = clock_get_hz(clk_sys) / div / 128.0;
      if (fabs(fs - FS) < FS * 0.01) best = fs;
    }
  }
  return best;
}

static void sendFrame(uint8_t type, const uint8_t *payload, uint16_t len, uint8_t sum) {
  uint8_t hdr[5] = { 0xA5, 0x5A, type, (uint8_t)(len & 0xff), (uint8_t)(len >> 8) };
  Serial.write(hdr, 5);
  Serial.write(payload, len);
  Serial.write(&sum, 1);
}

static void sendTick(const TickSlot &s) {
  // Same layout as the ESP32 WebSocket frame: u8 type=1, u8 ver=1, u16 n,
  // u32 pre, f64 onset, f32 peak, f32 floor, f32 thr, then u16 env[n].
  uint8_t hdr[28];
  hdr[0] = 1; hdr[1] = 1;
  uint16_t n = SNAP; memcpy(hdr + 2, &n, 2);
  uint32_t pre = PRE; memcpy(hdr + 4, &pre, 4);
  memcpy(hdr + 8, &s.onset, 8);
  memcpy(hdr + 16, &s.peak, 4);
  memcpy(hdr + 20, &s.floor, 4);
  memcpy(hdr + 24, &s.thr, 4);
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  const uint8_t *env = (const uint8_t *)s.env;
  for (int i = 0; i < 2 * SNAP; i++) sum += env[i];
  uint16_t len = sizeof(hdr) + 2 * SNAP;
  uint8_t fh[5] = { 0xA5, 0x5A, 1, (uint8_t)(len & 0xff), (uint8_t)(len >> 8) };
  Serial.write(fh, 5);
  Serial.write(hdr, sizeof(hdr));
  Serial.write(env, 2 * SNAP);
  Serial.write(&sum, 1);
}

static void applyConfig(const char *json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  if (doc["hpf"].is<float>())  cfgHpfHz = constrain(doc["hpf"].as<float>(), 200.0f, 10000.0f);
  if (doc["sens"].is<float>()) cfgSens = constrain(doc["sens"].as<float>(), 1.5f, 100.0f);
  if (doc["minThr"].is<float>()) cfgMinThr = constrain(doc["minThr"].as<float>(), 1e-6f, 0.5f);
  if (doc["refractoryMs"].is<float>()) cfgRefractoryMs = constrain(doc["refractoryMs"].as<float>(), 10.0f, 400.0f);
  cfgDirty = true;
}

// ---------------------------------------------------------------- core 0: USB
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  static char line[256];
  static size_t lineLen = 0;
  static uint32_t lastStats = 0, lastTicks = 0, lastTickMs = 0x80000000;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) { line[lineLen] = 0; applyConfig(line); lineLen = 0; }
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }

  // LED: a short flash per tick while ticks are coming in (handy for
  // positioning the watch), otherwise solid on with a host connected, or a
  // slow heartbeat so you can see the board is alive.
  uint32_t now = millis();
  bool host = Serial;
  if (stTicks != lastTicks) { lastTicks = stTicks; lastTickMs = now; }
  bool led;
  if (now - lastTickMs < 1000) led = now - lastTickMs < 30;
  else if (host) led = true;
  else led = now % 1000 < 100;
  digitalWrite(LED_BUILTIN, led);

  while (slotTail != slotHead) {
    if (host) sendTick(slots[slotTail % SLOTS]);
    __dmb();
    slotTail = slotTail + 1;
  }

  if (host && core1Ready && now - lastStats >= 500) {
    lastStats = now;
    char json[320];
    int n = snprintf(json, sizeof(json),
      "{\"t\":\"stats\",\"fs\":%.6f,\"samples\":%llu,\"peak\":%.6g,\"floor\":%.6g,\"thr\":%.6g,"
      "\"ch\":%d,\"ticks\":%lu,\"dropped\":%lu,\"overruns\":%lu,\"board\":\"pico\","
      "\"cfg\":{\"hpf\":%.0f,\"sens\":%.2f,\"minThr\":%.6g,\"refractoryMs\":%.1f}}",
      fsActual, (unsigned long long)stSamples, stPeak / FULL_SCALE, stFloor / FULL_SCALE,
      stThr / FULL_SCALE, stChannel, (unsigned long)stTicks, (unsigned long)stDropped,
      (unsigned long)stOverruns, cfgHpfHz, cfgSens, cfgMinThr, cfgRefractoryMs);
    stPeak = 0;
    if (n > 0 && n < (int)sizeof(json)) {
      uint8_t sum = 0;
      for (int i = 0; i < n; i++) sum += (uint8_t)json[i];
      sendFrame(2, (const uint8_t *)json, n, sum);
    }
  }
}

// ---------------------------------------------------------------- core 1: audio + detector
static int32_t ring[RING];

void setup1() {
  i2s.setBCLK(PIN_SCK);
  i2s.setDATA(PIN_SD);
  i2s.setBitsPerSample(32);
  i2s.setFrequency(FS);
  i2s.setBuffers(8, 256);
  i2s.begin();
  fsActual = measureI2sRate();
  core1Ready = true;
}

void loop1() {
  BiquadQ hp1, hp2;
  int32_t env = 0, prevEnv = 0;
  int64_t floorQ = (int64_t)500 << 20;   // noise floor, 20 fractional bits
  uint64_t idx = 0, lastOnset = 0;
  bool haveOnset = false;
  struct { uint64_t onsetIdx; double onset; int32_t floor, thr; } pending[4];
  int nPending = 0;
  int64_t energy[2] = { 0, 0 };
  int ch = 0;
  uint32_t chCount = 0;
  int32_t peakSinceStat = 0;
  int32_t sensQ8 = 0, minThr = 0, refr = 0;

  for (;;) {
    if (cfgDirty) {
      cfgDirty = false;
      // 4th-order Butterworth high-pass as two biquads
      hp1.highpass(cfgHpfHz, 0.5412f);
      hp2.highpass(cfgHpfHz, 1.3066f);
      sensQ8 = (int32_t)(cfgSens * 256.0f);
      minThr = (int32_t)(cfgMinThr * FULL_SCALE);
      refr = (int32_t)(cfgRefractoryMs * FS / 1000.0f);
    }
    if (i2s.getOverUnderflow()) stOverruns++;

    // one block of 256 frames between bookkeeping passes
    for (int f = 0; f < 256; f++) {
      int32_t l, r;
      i2s.read32(&l, &r);
      l >>= 8; r >>= 8;   // 24-bit samples, MSB-aligned in 32
      energy[0] += (int64_t)(l >> 4) * (l >> 4);
      energy[1] += (int64_t)(r >> 4) * (r >> 4);
      int32_t y = hp2.run(hp1.run(ch ? r : l));
      int32_t a = y < 0 ? -y : y;
      int32_t d = env - (env >> 5) - (env >> 7);   // ~0.5 ms peak-hold release
      env = a > d ? a : d;
      ring[idx & (RING - 1)] = env;

      // asymmetric noise-floor tracker: ~21 ms down, ~2.7 s up
      floorQ += (((int64_t)env << 20) - floorQ) >> (((int64_t)env << 20) < floorQ ? 10 : 17);
      int32_t floorLvl = (int32_t)(floorQ >> 20);
      int32_t thr = (int32_t)(((int64_t)floorLvl * sensQ8) >> 8);
      if (thr < minThr) thr = minThr;
      if (env > peakSinceStat) peakSinceStat = env;

      if (env >= thr && prevEnv < thr && (!haveOnset || idx - lastOnset > (uint64_t)refr) && idx > PRE + 4800) {
        haveOnset = true;
        lastOnset = idx;
        if (nPending < 4) {
          auto &p = pending[nPending++];
          p.onsetIdx = idx;
          p.onset = (double)(idx - 1) + (double)(thr - prevEnv) / (double)(env - prevEnv);
          p.floor = floorLvl;
          p.thr = thr;
        }
        stTicks = stTicks + 1;
      }
      prevEnv = env;

      if (nPending && idx >= pending[0].onsetIdx + POST - 1) {
        if (slotHead - slotTail < SLOTS) {
          TickSlot &s = slots[slotHead % SLOTS];
          uint64_t start = pending[0].onsetIdx - PRE;
          int32_t peak = 1;
          for (int i = 0; i < SNAP; i++) { int32_t v = ring[(start + i) & (RING - 1)]; if (v > peak) peak = v; }
          uint32_t scale = (uint32_t)(((uint64_t)65535 << 16) / (uint32_t)peak);
          for (int i = 0; i < SNAP; i++)
            s.env[i] = (uint16_t)(((uint64_t)ring[(start + i) & (RING - 1)] * scale + 0x8000) >> 16);
          s.onset = pending[0].onset;
          s.peak = peak / FULL_SCALE;
          s.floor = pending[0].floor / FULL_SCALE;
          s.thr = pending[0].thr / FULL_SCALE;
          __dmb();
          slotHead = slotHead + 1;
        } else {
          stDropped = stDropped + 1;
        }
        for (int i = 1; i < nPending; i++) pending[i - 1] = pending[i];
        nPending--;
      }
      idx++;
    }

    // Re-pick the live slot once a second (the unused one reads ~0)
    chCount += 256;
    if (chCount >= (uint32_t)FS) {
      int best = energy[1] > energy[0] * 4 ? 1 : (energy[0] > energy[1] * 4 ? 0 : ch);
      if (best != ch) { ch = best; env = prevEnv = 0; }
      stChannel = ch;
      energy[0] = energy[1] = 0;
      chCount = 0;
    }
    stSamples = idx;
    stFloor = (int32_t)(floorQ >> 20);
    int32_t thrNow = (int32_t)(((int64_t)stFloor * sensQ8) >> 8);
    stThr = thrNow < minThr ? minThr : thrNow;
    if (peakSinceStat > stPeak) stPeak = peakSinceStat;
    peakSinceStat = 0;
  }
}
