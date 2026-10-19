// ESP32 + INMP441 Timegrapher
//
// The ESP32 does the time-critical part: it samples the INMP441 at 48 kHz,
// band-limits the signal, finds the onset of every tick and timestamps it
// against the I2S sample clock (crystal-derived, so far better than any
// software timer). Each tick is streamed over a WebSocket together with a
// 40 ms snapshot of its envelope. The browser UI (served from this device)
// does BPH detection, rate / beat error / amplitude maths and drawing.
//
// Wiring (classic ESP32 DevKit):
//   INMP441 VDD -> 3V3      INMP441 GND -> GND
//   INMP441 L/R -> GND      (left channel; firmware auto-detects either)
//   INMP441 SCK -> GPIO32   INMP441 WS  -> GPIO25   INMP441 SD -> GPIO33

#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_http_server.h>
#include <driver/i2s_std.h>
#include <ArduinoJson.h>
#include <math.h>
#include <atomic>
#include "index_html.h"

// ---------------------------------------------------------------- config
#define I2S_SCK_PIN 32
#define I2S_WS_PIN  25
#define I2S_SD_PIN  33

// Leave STA_SSID empty to always run as an access point. If set, the device
// joins that network and falls back to its own AP when it can't connect.
static const char *STA_SSID = "";
static const char *STA_PASS = "";
static const char *AP_SSID  = "Timegrapher";
static const char *AP_PASS  = "timegrapher";   // min 8 chars
static const char *HOSTNAME = "timegrapher";    // http://timegrapher.local

static const uint32_t FS = 48000;

// Envelope snapshot sent with every tick: 2 ms before onset, 38 ms after.
static const int PRE  = FS * 2 / 1000;
static const int POST = FS * 38 / 1000;
static const int SNAP = PRE + POST;
static const int RING = 4096;   // power of two, > SNAP
static const int MAX_PENDING = 4;
static const int MAX_INFLIGHT = 6;   // WS messages queued but not yet sent

// ---------------------------------------------------------------- tunables (set from UI)
static volatile float cfgHpfHz      = 2000.0f; // high-pass corner
static volatile float cfgSens       = 6.0f;    // threshold = floor * sens
static volatile float cfgMinThr     = 0.0002f; // absolute floor for threshold (full scale = 1)
static volatile float cfgRefractory = 0.040f;  // s, ignore new onsets this soon after one
static volatile bool  cfgDirty      = true;

// ---------------------------------------------------------------- status (read by loop)
static volatile uint64_t stSamples = 0;
static volatile float stFloor = 0, stThr = 0, stPeak = 0;
static volatile uint32_t stTicks = 0, stDropped = 0;
static volatile int stChannel = -1;
static volatile int64_t stStartUs = 0;

static httpd_handle_t server = nullptr;
static std::atomic<int> inFlight{0};

// ---------------------------------------------------------------- DSP types
// (declared before any function so Arduino's generated prototypes can see them)
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void highpass(float fc, float q) {
    float w0 = 2.0f * (float)M_PI * fc / FS;
    float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f + c) / 2.0f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
  }
  inline float run(float x) {   // transposed direct form II
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

struct Pending {
  uint64_t onsetIdx;   // sample index of first sample at/above threshold
  double onset;        // interpolated onset, in samples
  float floor, thr;
};

// ---------------------------------------------------------------- WebSocket broadcast
struct WsMsg {
  httpd_ws_type_t type;
  size_t len;
  uint8_t data[];
};

static void wsBroadcastWork(void *arg) {
  WsMsg *m = (WsMsg *)arg;
  int fds[16];
  size_t n = 16;
  if (httpd_get_client_list(server, &n, fds) == ESP_OK) {
    for (size_t i = 0; i < n; i++) {
      if (httpd_ws_get_fd_info(server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
      httpd_ws_frame_t f = {};
      f.final = true;
      f.type = m->type;
      f.payload = m->data;
      f.len = m->len;
      httpd_ws_send_frame_async(server, fds[i], &f);
    }
  }
  free(m);
  inFlight--;
}

// Takes ownership of m.
static void wsBroadcast(WsMsg *m) {
  if (!server || inFlight >= MAX_INFLIGHT) { free(m); stDropped++; return; }
  inFlight++;
  if (httpd_queue_work(server, wsBroadcastWork, m) != ESP_OK) { inFlight--; free(m); }
}

static WsMsg *wsAlloc(httpd_ws_type_t type, size_t len) {
  WsMsg *m = (WsMsg *)malloc(sizeof(WsMsg) + len);
  if (m) { m->type = type; m->len = len; }
  return m;
}

// ---------------------------------------------------------------- audio
static i2s_chan_handle_t rxChan;

static void i2sInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  ESP_ERROR_CHECK(i2s_new_channel(&cc, nullptr, &rxChan));

  // Read both slots: depending on chip and the L/R strap the mic may appear in
  // either one. The audio task picks whichever carries signal.
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FS),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK_PIN,
      .ws = (gpio_num_t)I2S_WS_PIN,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)I2S_SD_PIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rxChan, &sc));
  ESP_ERROR_CHECK(i2s_channel_enable(rxChan));
}

static void sendTick(const Pending &p, const float *ring) {
  // Layout (little endian): u8 type=1, u8 ver=1, u16 n, u32 pre, f64 onset,
  // f32 peak, f32 floor, f32 thr, u16 env[n] (scaled so peak = 65535)
  const size_t hdr = 28, len = hdr + 2 * SNAP;
  WsMsg *m = wsAlloc(HTTPD_WS_TYPE_BINARY, len);
  if (!m) { stDropped++; return; }
  uint64_t start = p.onsetIdx - PRE;
  float peak = 1e-9f;
  for (int i = 0; i < SNAP; i++) peak = fmaxf(peak, ring[(start + i) & (RING - 1)]);
  uint8_t *d = m->data;
  d[0] = 1; d[1] = 1;
  uint16_t n = SNAP; memcpy(d + 2, &n, 2);
  uint32_t pre = PRE; memcpy(d + 4, &pre, 4);
  memcpy(d + 8, &p.onset, 8);
  memcpy(d + 16, &peak, 4);
  memcpy(d + 20, &p.floor, 4);
  memcpy(d + 24, &p.thr, 4);
  uint16_t *env = (uint16_t *)(d + hdr);
  float k = 65535.0f / peak;
  for (int i = 0; i < SNAP; i++) env[i] = (uint16_t)lrintf(ring[(start + i) & (RING - 1)] * k);
  wsBroadcast(m);
}

static void audioTask(void *) {
  static int32_t buf[256 * 2];
  static float ring[RING];
  Biquad hp1, hp2;
  Pending pending[MAX_PENDING];
  int nPending = 0;

  // same shift-based constants as the fixed-point Pico firmware
  const float envDecay = 1.0f - 1.0f / 32 - 1.0f / 128;  // ~0.5 ms peak-hold release
  const float floorDown = 1.0f / 1024;                    // ~21 ms
  const float floorUp   = 1.0f / 131072;                  // ~2.7 s

  float env = 0, floorLvl = 1e-4f, prevEnv = 0;
  uint64_t idx = 0, lastOnset = 0;
  bool haveOnset = false;
  double energy[2] = {0, 0};
  int ch = 0;
  uint32_t chCount = 0;
  float peakSinceStat = 0;

  stStartUs = esp_timer_get_time();

  for (;;) {
    if (cfgDirty) {
      cfgDirty = false;
      // 4th-order Butterworth high-pass as two biquads
      hp1.highpass(cfgHpfHz, 0.5412f);
      hp2.highpass(cfgHpfHz, 1.3066f);
    }
    size_t got = 0;
    if (i2s_channel_read(rxChan, buf, sizeof(buf), &got, portMAX_DELAY) != ESP_OK) continue;
    int frames = got / 8;
    const float refr = cfgRefractory * FS, sens = cfgSens, minThr = cfgMinThr;

    for (int f = 0; f < frames; f++) {
      float l = (float)(buf[2 * f] >> 8) * (1.0f / 8388608.0f);
      float r = (float)(buf[2 * f + 1] >> 8) * (1.0f / 8388608.0f);
      energy[0] += l * l;
      energy[1] += r * r;
      float x = ch ? r : l;

      float y = hp2.run(hp1.run(x));
      float a = fabsf(y);
      env = a > env * envDecay ? a : env * envDecay;
      ring[idx & (RING - 1)] = env;

      floorLvl += (env < floorLvl ? floorDown : floorUp) * (env - floorLvl);
      float thr = fmaxf(floorLvl * sens, minThr);
      if (env > peakSinceStat) peakSinceStat = env;

      if (env >= thr && prevEnv < thr && (!haveOnset || (idx - lastOnset) > refr) && idx > PRE + 4800) {
        haveOnset = true;
        lastOnset = idx;
        if (nPending < MAX_PENDING) {
          Pending &p = pending[nPending++];
          p.onsetIdx = idx;
          p.onset = (double)(idx - 1) + (double)((thr - prevEnv) / (env - prevEnv));
          p.floor = floorLvl;
          p.thr = thr;
        }
        stTicks++;
      }
      prevEnv = env;

      if (nPending && idx >= pending[0].onsetIdx + POST - 1) {
        sendTick(pending[0], ring);
        for (int i = 1; i < nPending; i++) pending[i - 1] = pending[i];
        nPending--;
      }
      idx++;
    }

    // Re-pick the live channel once a second (the unused slot reads ~0)
    chCount += frames;
    if (chCount >= FS) {
      int best = energy[1] > energy[0] * 4 ? 1 : (energy[0] > energy[1] * 4 ? 0 : ch);
      if (best != ch) { ch = best; env = prevEnv = 0; }
      stChannel = ch;
      energy[0] = energy[1] = 0;
      chCount = 0;
    }
    stSamples = idx;
    stFloor = floorLvl;
    stThr = fmaxf(floorLvl * sens, minThr);
    if (peakSinceStat > stPeak) stPeak = peakSinceStat;
    peakSinceStat = 0;
  }
}

// ---------------------------------------------------------------- HTTP / WS handlers
static void applyConfig(const char *json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  if (doc["hpf"].is<float>())  cfgHpfHz = constrain(doc["hpf"].as<float>(), 200.0f, 10000.0f);
  if (doc["sens"].is<float>()) cfgSens = constrain(doc["sens"].as<float>(), 1.5f, 100.0f);
  if (doc["minThr"].is<float>()) cfgMinThr = constrain(doc["minThr"].as<float>(), 1e-6f, 0.5f);
  if (doc["refractoryMs"].is<float>())
    cfgRefractory = constrain(doc["refractoryMs"].as<float>(), 10.0f, 400.0f) / 1000.0f;
  cfgDirty = true;
}

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    Serial.println("WS client connected");
    return ESP_OK;
  }
  httpd_ws_frame_t f = {};
  esp_err_t r = httpd_ws_recv_frame(req, &f, 0);
  if (r != ESP_OK) return r;
  if (f.len == 0) return ESP_OK;
  if (f.len > 512) return ESP_FAIL;
  uint8_t buf[513];
  f.payload = buf;
  r = httpd_ws_recv_frame(req, &f, f.len);
  if (r != ESP_OK) return r;
  buf[f.len] = 0;
  if (f.type == HTTPD_WS_TYPE_TEXT) applyConfig((const char *)buf);
  return ESP_OK;
}

static void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.lru_purge_enable = true;
  if (httpd_start(&server, &cfg) != ESP_OK) { Serial.println("httpd_start failed"); return; }
  httpd_uri_t index = {};
  index.uri = "/";
  index.method = HTTP_GET;
  index.handler = indexHandler;
  httpd_register_uri_handler(server, &index);
  httpd_uri_t ws = {};
  ws.uri = "/ws";
  ws.method = HTTP_GET;
  ws.handler = wsHandler;
  ws.is_websocket = true;
  httpd_register_uri_handler(server, &ws);
}

static void startWifi() {
  WiFi.setHostname(HOSTNAME);
  if (strlen(STA_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASS);
    Serial.printf("Joining %s", STA_SSID);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);
      Serial.printf("Open http://%s/  or  http://%s.local/\n", WiFi.localIP().toString().c_str(), HOSTNAME);
      return;
    }
    Serial.println("Could not join, starting access point instead");
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  Serial.printf("Join WiFi \"%s\" (password \"%s\"), then open http://%s/\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32 Timegrapher");
  startWifi();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  startServer();
  i2sInit();
#if CONFIG_FREERTOS_UNICORE
  const int core = 0;
#else
  const int core = 1;
#endif
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 10, nullptr, core);
}

void loop() {
  static uint32_t last = 0, lastLog = 0;
  uint32_t now = millis();
  if (now - last < 500) { delay(10); return; }
  last = now;

  float elapsed = (esp_timer_get_time() - stStartUs) / 1e6f;
  float fsMeas = elapsed > 1 ? stSamples / elapsed : 0;
  char json[320];
  int n = snprintf(json, sizeof(json),
    "{\"t\":\"stats\",\"fs\":%u,\"fsMeas\":%.1f,\"samples\":%llu,\"peak\":%.6g,\"floor\":%.6g,"
    "\"thr\":%.6g,\"ch\":%d,\"ticks\":%u,\"dropped\":%u,\"heap\":%u,"
    "\"cfg\":{\"hpf\":%.0f,\"sens\":%.2f,\"minThr\":%.6g,\"refractoryMs\":%.1f}}",
    (unsigned)FS, fsMeas, (unsigned long long)stSamples, stPeak, stFloor, stThr, stChannel,
    (unsigned)stTicks, (unsigned)stDropped, (unsigned)ESP.getFreeHeap(),
    cfgHpfHz, cfgSens, cfgMinThr, cfgRefractory * 1000.0f);
  stPeak = 0;
  if (n > 0 && n < (int)sizeof(json)) {
    WsMsg *m = wsAlloc(HTTPD_WS_TYPE_TEXT, n);
    if (m) { memcpy(m->data, json, n); wsBroadcast(m); }
  }
  if (now - lastLog >= 5000) {
    lastLog = now;
    Serial.printf("ch=%d floor=%.6f thr=%.6f ticks=%u fs=%.1f heap=%u\n",
                  stChannel, stFloor, stThr, (unsigned)stTicks, fsMeas, (unsigned)ESP.getFreeHeap());
  }
}
