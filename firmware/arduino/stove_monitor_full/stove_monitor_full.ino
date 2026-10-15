#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <erenpaper-project-1_inferencing.h>
#pragma GCC diagnostic pop

#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include <string.h>

// ================================================================
// CREDENTIALS live in secrets.h, which is NOT in this repository.
// Copy secrets.example.h to secrets.h and fill in your own Wi-Fi and
// AWS IoT values. secrets.h is git-ignored.
// ================================================================
#include "secrets.h"

#define MQTT_TOPIC  "home/sensors/stove"
#define THING_NAME  "stove_pico"

// ================================================================
// SENSOR PINS
// ================================================================
#define PIR_PIN              16
#define DS18_PIN             15
#define FEATURES_PER_SAMPLE  3
#define SAMPLE_INTERVAL_MS   1750

// ================================================================
// BURNER THRESHOLDS
// ================================================================
#define BURNER_HOT_THRESHOLD_C    65.0f
#define BURNER_ATTENDED_MOTION    3
#define BURNER_WARMUP_RISE_C      2.0f
#define BURNER_ROOM_TEMP_C        30.0f
#define BURNER_UNATTENDED_ALERT   25

// ================================================================
// OVEN THRESHOLDS (derived from real data)
// ================================================================
#define OVEN_HOT_THRESHOLD_C      62.0f
#define OVEN_WARM_THRESHOLD_C     32.0f
#define OVEN_COOLING_ROOM_C       24.0f
#define OVEN_HOT_ALERT_SEC        10800  // 3 hours on-device alert

// ================================================================
// DEBOUNCE
// ================================================================
#define DEBOUNCE_COUNT  2

// ================================================================
// SENSORS
// ================================================================
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
OneWire oneWire(DS18_PIN);
DallasTemperature ds(&oneWire);
DeviceAddress dsAddr;
bool dsFound = false;

// ================================================================
// WiFi / MQTT
// ================================================================
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long lastWifiRetry = 0;

// ================================================================
// OFFLINE EVENT QUEUE
// ================================================================
#define EVENT_QUEUE_SIZE 10
String eventQueue[EVENT_QUEUE_SIZE];
int queueHead = 0;
int queueTail = 0;

// ================================================================
// NTP TIMESTAMP
// ================================================================
unsigned long ntp_time_at_sync = 0;
unsigned long millis_at_sync   = 0;

unsigned long getAccurateTime() {
  return ntp_time_at_sync + (millis() - millis_at_sync) / 1000;
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("NTP syncing");
  time_t now = 0; int retries = 0;
  while (now < 1000000000 && retries < 20) {
    delay(500); Serial.print("."); time(&now); retries++;
  }
  ntp_time_at_sync = (unsigned long)time(nullptr);
  millis_at_sync   = millis();
  Serial.println(now > 1000000000 ? " synced" : " FAILED");
}

// ================================================================
// EI INFERENCE GLOBALS (burner)
// ================================================================
float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
size_t feature_ix       = 0;
unsigned long last_sample_ms  = 0;
unsigned long sample_count    = 0;
unsigned long inference_count = 0;

// Burner cooling session context
static bool  burner_in_cooling = false;
static float burner_peak_mlx   = 0.0f;

// ================================================================
// SHARED SENSOR READINGS (updated every sample)
// ================================================================
static float last_mlx = 0.0f;
static float last_ds  = 0.0f;
static int   last_pir = 0;

// ================================================================
// BURNER SESSION TRACKING
// ================================================================
static bool          burner_on                  = false;
static unsigned long burner_start_ms            = 0;
static const char*   burner_confirmed           = "burner_idle";
static const char*   burner_current             = "burner_idle";
static int           burner_debounce            = 0;
static int           burner_unattended_count    = 0;
static bool          burner_anomaly_sent        = false;
static unsigned long burner_state_entered_ms    = 0;
static unsigned long burner_warmup_sec          = 0;
static unsigned long burner_hot_attended_sec    = 0;
static unsigned long burner_hot_unattended_sec  = 0;
static float         burner_temp_at_start       = 0.0f;
static float         burner_ds_at_start         = 0.0f;
static float         burner_temp_at_stop        = 0.0f;
static float         burner_ds_at_stop          = 0.0f;
static float         burner_peak_session_mlx    = 0.0f;
static float         burner_peak_session_ds     = 0.0f;
static unsigned long burner_last_motion_ms      = 0;

// ================================================================
// OVEN SESSION TRACKING
// ================================================================
static bool          oven_on                = false;
static unsigned long oven_start_ms          = 0;
static const char*   oven_confirmed         = "oven_idle";
static const char*   oven_current           = "oven_idle";
static int           oven_debounce          = 0;
static bool          oven_in_cooling        = false;
static float         oven_peak_ds           = 0.0f;
static unsigned long oven_state_entered_ms  = 0;
static unsigned long oven_warmup_sec        = 0;
static unsigned long oven_hot_sec           = 0;
static float         oven_ds_at_start       = 0.0f;
static float         oven_ds_at_stop        = 0.0f;
static float         oven_mlx_at_stop       = 0.0f;
static float         oven_peak_session_ds   = 0.0f;
static bool          oven_alert_sent        = false;

// DS baseline for slope detection (updated each inference window)
static float         oven_ds_window_start   = 0.0f;
static unsigned long oven_ds_window_count   = 0;

// ================================================================
// MQTT HELPERS
// ================================================================
void enqueueEvent(const char* payload) {
  eventQueue[queueHead] = payload;
  queueHead = (queueHead + 1) % EVENT_QUEUE_SIZE;
}

void flushEventQueue() {
  if (!mqtt.connected()) return;
  while (queueTail != queueHead) {
    if (mqtt.publish(MQTT_TOPIC, eventQueue[queueTail].c_str())) {
      Serial.print("mqtt: flushed: ");
      Serial.println(eventQueue[queueTail]);
      queueTail = (queueTail + 1) % EVENT_QUEUE_SIZE;
    } else break;
  }
}

void publishOrQueue(const char* payload) {
  if (mqtt.connected()) {
    mqtt.publish(MQTT_TOPIC, payload);
    Serial.print("mqtt: published: ");
    Serial.println(payload);
  } else {
    enqueueEvent(payload);
    Serial.print("mqtt: queued: ");
    Serial.println(payload);
  }
}

// ================================================================
// BURNER EMIT EVENT
// ================================================================
void emitBurnerEvent(const char* evtState) {
  char payload[512];
  unsigned long ts = getAccurateTime();

  if (strcmp(evtState, "start") == 0) {
    snprintf(payload, sizeof(payload),
      "{\"device_id\":\"stove\",\"activity\":\"burner_on\","
      "\"state\":\"start\",\"timestamp_ms\":%lu000,"
      "\"temp_at_start\":%.1f,\"ds_temp_at_start\":%.1f}",
      ts, burner_temp_at_start, burner_ds_at_start);
  } else {
    unsigned long duration       = (millis() - burner_start_ms) / 1000;
    unsigned long last_motion_ago = (burner_last_motion_ms > 0)
                                    ? (millis() - burner_last_motion_ms) / 1000
                                    : duration;
    snprintf(payload, sizeof(payload),
      "{\"device_id\":\"stove\",\"activity\":\"burner_on\","
      "\"state\":\"stop\",\"timestamp_ms\":%lu000,"
      "\"duration_sec\":%lu,\"warmup_duration_sec\":%lu,"
      "\"hot_attended_sec\":%lu,\"hot_unattended_sec\":%lu,"
      "\"peak_temp_c\":%.1f,\"peak_ds_temp_c\":%.1f,"
      "\"state_at_stop\":\"%s\",\"temp_at_stop\":%.1f,"
      "\"ds_temp_at_stop\":%.1f,\"last_motion_sec_ago\":%lu}",
      ts, duration,
      burner_warmup_sec, burner_hot_attended_sec, burner_hot_unattended_sec,
      burner_peak_session_mlx, burner_peak_session_ds,
      burner_confirmed,
      burner_temp_at_stop, burner_ds_at_stop, last_motion_ago);
  }
  publishOrQueue(payload);
}

// ================================================================
// OVEN EMIT EVENT
// ================================================================
void emitOvenEvent(const char* evtState) {
  char payload[512];
  unsigned long ts = getAccurateTime();

  if (strcmp(evtState, "start") == 0) {
    snprintf(payload, sizeof(payload),
      "{\"device_id\":\"stove\",\"activity\":\"oven_on\","
      "\"state\":\"start\",\"timestamp_ms\":%lu000,"
      "\"ds_temp_at_start\":%.1f,\"temp_at_start\":%.1f}",
      ts, oven_ds_at_start, last_mlx);
  } else {
    unsigned long duration = (millis() - oven_start_ms) / 1000;
    snprintf(payload, sizeof(payload),
      "{\"device_id\":\"stove\",\"activity\":\"oven_on\","
      "\"state\":\"stop\",\"timestamp_ms\":%lu000,"
      "\"duration_sec\":%lu,\"warmup_duration_sec\":%lu,"
      "\"hot_duration_sec\":%lu,"
      "\"peak_ds_temp_c\":%.1f,\"state_at_stop\":\"%s\","
      "\"ds_temp_at_stop\":%.1f,\"temp_at_stop\":%.1f}",
      ts, duration,
      oven_warmup_sec, oven_hot_sec,
      oven_peak_session_ds, oven_confirmed,
      oven_ds_at_stop, oven_mlx_at_stop);
  }
  publishOrQueue(payload);
}

// ================================================================
// WIFI / MQTT CONNECTION
// ================================================================
void ensureConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, LOW); // off = no WiFi
    if (millis() - lastWifiRetry > 10000) {
      lastWifiRetry = millis();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }
  if (ntp_time_at_sync == 0) {
    syncTime();
  }
  if (!mqtt.connected()) {
    digitalWrite(LED_BUILTIN, (millis() % 500 < 250)); // slow blink = no MQTT
    if (mqtt.connect(THING_NAME)) {
      Serial.println("mqtt: reconnected");
      flushEventQueue();
    }
  }
  if (mqtt.connected()) {
    digitalWrite(LED_BUILTIN, HIGH); // solid on = fully connected
    mqtt.loop();
    flushEventQueue();
  }
}

// ================================================================
// BURNER — getFinalState (v2 with cooling session tracking)
// ================================================================
const char* getBurnerState(const char* eiLabel, float bestValue,
                           float baseline_mlx, float current_mlx,
                           int motion_count) {
  float mlx_rise = current_mlx - baseline_mlx;

  if (current_mlx > burner_peak_mlx) burner_peak_mlx = current_mlx;
  if (!burner_in_cooling && burner_peak_mlx >= BURNER_HOT_THRESHOLD_C
      && current_mlx < BURNER_HOT_THRESHOLD_C)
    burner_in_cooling = true;
  if (burner_in_cooling && current_mlx <= BURNER_ROOM_TEMP_C) {
    burner_in_cooling = false;
    burner_peak_mlx = current_mlx;
  }

  bool started_near_room   = (baseline_mlx >= 18.0f && baseline_mlx <= 35.0f);
  bool warming_trend       = (mlx_rise >= BURNER_WARMUP_RISE_C);
  bool meaningful_cooling  = (mlx_rise <= -0.5f);
  bool started_warm_or_hot = (baseline_mlx >= 40.0f);
  bool still_clearly_hot   = (current_mlx >= BURNER_HOT_THRESHOLD_C);

  bool rapid_rise = (mlx_rise >= 3.0f);
  if (rapid_rise && current_mlx >= BURNER_HOT_THRESHOLD_C) {
    burner_in_cooling = false; burner_peak_mlx = current_mlx;
    return (motion_count >= BURNER_ATTENDED_MOTION) ? "burner_hot_attended" : "burner_hot_unattended";
  }
  if (rapid_rise && current_mlx >= 28.0f) {
    burner_in_cooling = false;
    return "burner_warming_up";
  }
  if (burner_in_cooling && mlx_rise <= -1.0f && current_mlx >= BURNER_HOT_THRESHOLD_C)
    return "burner_cooling_down";
  if (burner_in_cooling && strcmp(eiLabel, "burner_cooling_down") == 0
      && bestValue >= 0.90f && mlx_rise <= -1.0f)
    return "burner_cooling_down";
  if (started_warm_or_hot && meaningful_cooling && !still_clearly_hot) {
    if (burner_in_cooling || strcmp(eiLabel, "burner_cooling_down") == 0)
      return "burner_cooling_down";
  }
  if (current_mlx >= BURNER_HOT_THRESHOLD_C)
    return (motion_count >= BURNER_ATTENDED_MOTION) ? "burner_hot_attended" : "burner_hot_unattended";
  if (current_mlx >= 58.0f && current_mlx < BURNER_HOT_THRESHOLD_C) {
    if (strcmp(eiLabel, "burner_hot_attended") == 0 && motion_count >= BURNER_ATTENDED_MOTION)
      return "burner_hot_attended";
    if (strcmp(eiLabel, "burner_hot_unattended") == 0 && motion_count < BURNER_ATTENDED_MOTION)
      return "burner_hot_unattended";
  }
  if (mlx_rise >= 1.0f && motion_count >= 1 && current_mlx >= 20.0f
      && current_mlx < BURNER_HOT_THRESHOLD_C && !burner_in_cooling)
    return "burner_warming_up";
  if (started_near_room && warming_trend)
    return "burner_warming_up";
  if (strcmp(eiLabel, "burner_warming_up") == 0)
    return "burner_warming_up";
  if (burner_in_cooling && strcmp(eiLabel, "burner_cooling_down") == 0 && meaningful_cooling)
    return "burner_cooling_down";
  if (current_mlx >= 28.0f && mlx_rise > 0.5f)
    return (current_mlx < BURNER_HOT_THRESHOLD_C) ? "burner_warming_up" : "burner_hot_unattended";
  if (current_mlx >= 35.0f &&
      !(burner_in_cooling && strcmp(eiLabel, "burner_cooling_down") == 0))
    return (current_mlx < BURNER_HOT_THRESHOLD_C) ? "burner_warming_up" : "burner_hot_unattended";
  return "burner_idle";
}

// ================================================================
// OVEN — getOvenState (pure rule-based from DS18B20)
// ================================================================
const char* getOvenState(float current_ds, float ds_rise) {
  if (current_ds > oven_peak_ds) oven_peak_ds = current_ds;

  // Enter cooling when DS drops below hot threshold after being hot
  if (!oven_in_cooling && oven_peak_ds >= OVEN_HOT_THRESHOLD_C
      && current_ds < OVEN_HOT_THRESHOLD_C)
    oven_in_cooling = true;

  // Exit cooling when back to room temp
  if (oven_in_cooling && current_ds <= OVEN_COOLING_ROOM_C) {
    oven_in_cooling = false;
    oven_peak_ds = current_ds;
  }

  // Hot — DS above hot threshold
  if (current_ds >= OVEN_HOT_THRESHOLD_C)
    return "oven_hot";

  // Cooling — was hot, now falling
  if (oven_in_cooling && ds_rise <= -0.05f)
    return "oven_cooling_down";

  // Still warm after being hot but not clearly falling — still call it cooling
  if (oven_in_cooling && current_ds > OVEN_COOLING_ROOM_C)
    return "oven_cooling_down";

  // Warming up — DS rising above room temp but not yet hot
  if (current_ds >= OVEN_WARM_THRESHOLD_C && ds_rise > 0.02f)
    return "oven_warming_up";

  // Warming up — between warm threshold and hot, even if slope is flat
  // (oven heats slowly, slope within one window is tiny)
  if (current_ds >= OVEN_WARM_THRESHOLD_C && current_ds < OVEN_HOT_THRESHOLD_C
      && oven_peak_ds >= OVEN_WARM_THRESHOLD_C)
    return "oven_warming_up";

  return "oven_idle";
}

// ================================================================
// BURNER STATE MACHINE
// ================================================================
void accumBurnerPrevState(const char* prev) {
  unsigned long t = (millis() - burner_state_entered_ms) / 1000;
  if (strcmp(prev, "burner_warming_up") == 0)       burner_warmup_sec += t;
  else if (strcmp(prev, "burner_hot_attended") == 0) burner_hot_attended_sec += t;
  else if (strcmp(prev, "burner_hot_unattended") == 0) burner_hot_unattended_sec += t;
  burner_state_entered_ms = millis();
}

void resetBurnerSession() {
  burner_warmup_sec = 0; burner_hot_attended_sec = 0; burner_hot_unattended_sec = 0;
  burner_peak_session_mlx = last_mlx; burner_peak_session_ds = last_ds;
  burner_temp_at_start = last_mlx; burner_ds_at_start = last_ds;
  burner_last_motion_ms = 0; burner_state_entered_ms = millis();
  burner_unattended_count = 0; burner_anomaly_sent = false;
}

void handleBurnerState(const char* newState) {
  // Debounce
  if (strcmp(newState, burner_current) == 0) burner_debounce++;
  else { burner_current = newState; burner_debounce = 1; return; }
  if (burner_debounce < DEBOUNCE_COUNT) return;

  // Update peaks
  if (last_mlx > burner_peak_session_mlx) burner_peak_session_mlx = last_mlx;
  if (last_ds  > burner_peak_session_ds)  burner_peak_session_ds  = last_ds;

  // Unattended counter
  if (strcmp(newState, "burner_hot_unattended") == 0) {
    burner_unattended_count++;
    if (burner_unattended_count >= BURNER_UNATTENDED_ALERT && !burner_anomaly_sent) {
      unsigned long t = (millis() - burner_state_entered_ms) / 1000;
      burner_temp_at_stop = last_mlx;
      burner_ds_at_stop   = last_ds;
      emitBurnerEvent("stop");
      burner_anomaly_sent = true;
      Serial.println("*** BURNER ANOMALY: unattended too long ***");
    }
  } else {
    burner_unattended_count = 0;
    burner_anomaly_sent = false;
  }

  if (strcmp(newState, burner_confirmed) == 0) return;

  const char* prev = burner_confirmed;
  accumBurnerPrevState(prev);
  burner_confirmed = newState;

  Serial.print("BURNER: "); Serial.print(prev);
  Serial.print(" -> "); Serial.println(newState);

  bool prev_idle  = (strcmp(prev, "burner_idle") == 0);
  bool now_active = (strcmp(newState, "burner_idle") != 0 &&
                     strcmp(newState, "burner_cooling_down") != 0);
  bool now_ending = (strcmp(newState, "burner_idle") == 0 ||
                     strcmp(newState, "burner_cooling_down") == 0);

  if (prev_idle && now_active && !burner_on) {
    burner_on = true; burner_start_ms = millis();
    resetBurnerSession();
    emitBurnerEvent("start");
  }
  if (now_ending && burner_on) {
    burner_on = false;
    burner_temp_at_stop = last_mlx;
    burner_ds_at_stop   = last_ds;
    emitBurnerEvent("stop");
  }
}

// ================================================================
// OVEN STATE MACHINE
// ================================================================
void accumOvenPrevState(const char* prev) {
  unsigned long t = (millis() - oven_state_entered_ms) / 1000;
  if (strcmp(prev, "oven_warming_up") == 0) oven_warmup_sec += t;
  else if (strcmp(prev, "oven_hot") == 0)   oven_hot_sec    += t;
  oven_state_entered_ms = millis();
}

void resetOvenSession() {
  oven_warmup_sec = 0; oven_hot_sec = 0;
  oven_peak_session_ds = last_ds;
  oven_ds_at_start     = last_ds;
  oven_state_entered_ms = millis();
  oven_alert_sent = false;
}

void handleOvenState(const char* newState) {
  // Debounce
  if (strcmp(newState, oven_current) == 0) oven_debounce++;
  else { oven_current = newState; oven_debounce = 1; return; }
  if (oven_debounce < DEBOUNCE_COUNT) return;

  // Update peak DS
  if (last_ds > oven_peak_session_ds) oven_peak_session_ds = last_ds;

  // Oven hot too long alert
  if (strcmp(newState, "oven_hot") == 0) {
    unsigned long hot_so_far = oven_hot_sec + (millis() - oven_state_entered_ms) / 1000;
    if (hot_so_far >= OVEN_HOT_ALERT_SEC && !oven_alert_sent) {
      oven_ds_at_stop  = last_ds;
      oven_mlx_at_stop = last_mlx;
      emitOvenEvent("stop");
      oven_alert_sent = true;
      Serial.println("*** OVEN ANOMALY: hot too long ***");
    }
  }

  if (strcmp(newState, oven_confirmed) == 0) return;

  const char* prev = oven_confirmed;
  accumOvenPrevState(prev);
  oven_confirmed = newState;

  Serial.print("OVEN: "); Serial.print(prev);
  Serial.print(" -> "); Serial.println(newState);

  bool prev_idle  = (strcmp(prev, "oven_idle") == 0);
  bool now_active = (strcmp(newState, "oven_idle") != 0 &&
                     strcmp(newState, "oven_cooling_down") != 0);
  bool now_ending = (strcmp(newState, "oven_idle") == 0 ||
                     strcmp(newState, "oven_cooling_down") == 0);

  if (prev_idle && now_active && !oven_on) {
    oven_on = true; oven_start_ms = millis();
    resetOvenSession();
    emitOvenEvent("start");
  }
  if (now_ending && oven_on) {
    oven_on = false;
    oven_ds_at_stop  = last_ds;
    oven_mlx_at_stop = last_mlx;
    emitOvenEvent("stop");
  }
}

// ================================================================
// SENSOR READ
// ================================================================
static int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
  memcpy(out_ptr, features + offset, length * sizeof(float));
  return 0;
}

bool readSensorSample(float *out) {
  float mlxTemp = NAN, dsTemp = NAN;
  last_pir = digitalRead(PIR_PIN);
  float obj = mlx.readObjectTempC();
  if (!isnan(obj)) mlxTemp = obj;
  ds.requestTemperatures();
  if (dsFound) {
    float t = ds.getTempC(dsAddr);
    if (t != DEVICE_DISCONNECTED_C) dsTemp = t;
  }
  if (isnan(mlxTemp)) mlxTemp = -999.0f;
  if (isnan(dsTemp))  dsTemp  = -999.0f;
  if (mlxTemp > -900) last_mlx = mlxTemp;
  if (dsTemp  > -900) last_ds  = dsTemp;
  out[0] = mlxTemp; out[1] = dsTemp; out[2] = (float)last_pir;
  return true;
}

// ================================================================
// INFERENCE (runs both burner EI + oven rules each window)
// ================================================================
void runInference() {
  inference_count++;

  // --- BURNER: run EI classifier ---
  signal_t signal;
  signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
  signal.get_data = &raw_feature_get_data;
  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    Serial.print("EI failed: "); Serial.println((int)err); return;
  }

  float bestValue = -1.0f;
  const char *bestLabel = "";
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > bestValue) {
      bestValue = result.classification[i].value;
      bestLabel = result.classification[i].label;
    }
  }

  float baseline_mlx = features[0];
  float current_mlx  = features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - FEATURES_PER_SAMPLE];
  float mlx_rise     = current_mlx - baseline_mlx;

  int motion_count = 0;
  for (size_t i = 2; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i += FEATURES_PER_SAMPLE)
    if (features[i] > 0.5f) motion_count++;

  if (motion_count > 0) burner_last_motion_ms = millis();

  const char* burnerState = getBurnerState(bestLabel, bestValue,
                                           baseline_mlx, current_mlx, motion_count);
  handleBurnerState(burnerState);

  // --- OVEN: rule-based from DS18B20 ---
  // DS baseline is first DS reading of window
  float baseline_ds = features[1];  // first sample ds_temp in window
  float current_ds  = last_ds;
  float ds_rise     = current_ds - baseline_ds;

  // Track DS window baseline for slope
  const char* ovenState = getOvenState(current_ds, ds_rise);
  handleOvenState(ovenState);

  // --- Serial debug ---
  Serial.print("["); Serial.print(inference_count); Serial.print("] ");
  Serial.print("BURNER="); Serial.print(burnerState);
  Serial.print(" OVEN="); Serial.print(ovenState);
  Serial.print(" mlx="); Serial.print(current_mlx, 1);
  Serial.print(" ds="); Serial.print(current_ds, 1);
  Serial.print(" ds_rise="); Serial.print(ds_rise, 3);
  Serial.print(" motion="); Serial.print(motion_count);
  Serial.print(" b_cool="); Serial.print(burner_in_cooling ? "Y" : "N");
  Serial.print(" o_cool="); Serial.println(oven_in_cooling ? "Y" : "N");
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 8000) delay(10);
  delay(1000);
  Serial.println("setup: starting");

  pinMode(PIR_PIN, INPUT);
  Wire.setSDA(0); Wire.setSCL(1); Wire.begin();

  if (!mlx.begin(0x5A, &Wire)) {
    Serial.println("ERROR: MLX not found"); while (1) delay(100);
  }
  Serial.println("setup: MLX ok");

  ds.begin();
  if (ds.getDeviceCount() > 0 && ds.getAddress(dsAddr, 0)) {
    dsFound = true; ds.setResolution(dsAddr, 12);
    Serial.println("setup: DS18B20 ok");
  } else Serial.println("setup: WARNING DS18B20 not found");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("setup: WiFi connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" connected");
    syncTime();
  } else Serial.println(" FAILED — offline mode");

  wifiClient.setCACert(ROOT_CA);
  wifiClient.setCertificate(DEVICE_CERT);
  wifiClient.setPrivateKey(PRIVATE_KEY);
  mqtt.setServer(AWS_ENDPOINT, 8883);
  mqtt.setBufferSize(512);

  if (WiFi.status() == WL_CONNECTED) {
    if (mqtt.connect(THING_NAME)) Serial.println("setup: MQTT connected");
    else Serial.println("setup: MQTT FAILED — will retry");
  }

  run_classifier_init();
  Serial.println("setup: warming sensors 30s...");
  delay(30000);
  Serial.println("setup: done");
  last_sample_ms = millis();
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  ensureConnected();

  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();

  float sample[FEATURES_PER_SAMPLE];
  if (!readSensorSample(sample)) return;

  sample_count++;
  for (size_t i = 0; i < FEATURES_PER_SAMPLE; i++)
    if (feature_ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)
      features[feature_ix++] = sample[i];

  if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    runInference();
    feature_ix = 0;
  }
}