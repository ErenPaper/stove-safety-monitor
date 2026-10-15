#include <erenpaper-project-1_inferencing.h>

#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <string.h>

#define PIR_PIN   16
#define DS18_PIN  15

#define FEATURES_PER_SAMPLE 3
#define SAMPLE_INTERVAL_MS 1750

#define HOT_THRESHOLD_C       65.0f
#define ATTENDED_MOTION_COUNT 3
#define WARMUP_RISE_C         2.0f   // FIX A: lowered from 4.0 — catches slow ignitions

const char* TEST_STAGE = "FULL TEST";

Adafruit_MLX90614 mlx = Adafruit_MLX90614();
OneWire oneWire(DS18_PIN);
DallasTemperature ds(&oneWire);
DeviceAddress dsAddr;
bool dsFound = false;

float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
size_t feature_ix = 0;
unsigned long last_sample_ms = 0;
unsigned long sample_count = 0;
unsigned long inference_count = 0;

static int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
  memcpy(out_ptr, features + offset, length * sizeof(float));
  return 0;
}

bool readSensorSample(float *out) {
  float mlxTemp = NAN;
  float dsTemp  = NAN;
  float motion  = digitalRead(PIR_PIN);

  float obj = mlx.readObjectTempC();
  if (!isnan(obj)) {
    mlxTemp = obj;
  }

  ds.requestTemperatures();
  if (dsFound) {
    float t = ds.getTempC(dsAddr);
    if (t != DEVICE_DISCONNECTED_C) {
      dsTemp = t;
    }
  }

  if (isnan(mlxTemp)) mlxTemp = -999.0f;
  if (isnan(dsTemp))  dsTemp  = -999.0f;

  // Order must match training CSV:
  // mlx_temp, ds_temp, motion
  out[0] = mlxTemp;
  out[1] = dsTemp;
  out[2] = motion;

  return true;
}

void printDivider() {
  Serial.println("==================================================");
}

const char* getFinalState(const char* eiLabel, float baseline_mlx, float current_mlx, int motion_count) {
  float mlx_rise = current_mlx - baseline_mlx;

  // FIX A: widened baseline window from 18-26 to 18-35
  // Covers kitchen-warm starts (27-34C) that previously fell through to idle
  bool started_near_room  = (baseline_mlx >= 18.0f && baseline_mlx <= 35.0f);
  bool warming_trend      = (mlx_rise >= WARMUP_RISE_C);  // now 2.0C threshold
  bool meaningful_cooling = (mlx_rise <= -0.5f);
  bool started_warm_or_hot = (baseline_mlx >= 40.0f);
  bool still_clearly_hot = (current_mlx >= HOT_THRESHOLD_C);

  // FIX 1: Rapid rise override — breaks out of stuck cooling state
  // If temp rose 3C+ in one window, the burner is clearly heating up
  // This must come FIRST before any other logic
  bool rapid_rise = (mlx_rise >= 3.0f);
  if (rapid_rise && current_mlx >= HOT_THRESHOLD_C) {
    return (motion_count >= ATTENDED_MOTION_COUNT) ? "burner_hot_attended" : "burner_hot_unattended";
  }
  if (rapid_rise && current_mlx >= 35.0f) {
    return "burner_warming_up_attended";
  }

  // Cooling detection: only if started warm/hot, dropped meaningfully,
  // and not still in the clearly-hot zone
  if (started_warm_or_hot && meaningful_cooling && !still_clearly_hot) {
    return "burner_cooling_down_unattended";
  }

  // Hot split by motion density
  if (current_mlx >= HOT_THRESHOLD_C) {
    if (motion_count >= ATTENDED_MOTION_COUNT) {
      return "burner_hot_attended";
    } else {
      return "burner_hot_unattended";
    }
  }

  // Bridge zone: upper warm band can become hot if EI strongly suggests it
  if (current_mlx >= 58.0f && current_mlx < HOT_THRESHOLD_C) {
    if (strcmp(eiLabel, "burner_hot_attended") == 0 && motion_count >= ATTENDED_MOTION_COUNT) {
      return "burner_hot_attended";
    }
    if (strcmp(eiLabel, "burner_hot_unattended") == 0 && motion_count < ATTENDED_MOTION_COUNT) {
      return "burner_hot_unattended";
    }
  }

  // FIX B: Motion-assisted warmup catch
  // Any detectable rise + at least 1 motion count above room temp = warming_up
  // True idle never sustains a rising MLX trend so this won't false-positive
  if (mlx_rise >= 1.5f && motion_count >= 1 && current_mlx >= 22.0f && current_mlx < HOT_THRESHOLD_C) {
    return "burner_warming_up_attended";
  }

  // Warmup below hot threshold — now catches wider baseline range and lower rise
  if (started_near_room && warming_trend) {
    return "burner_warming_up_attended";
  }

  // Trust EI if it clearly says warming_up
  if (strcmp(eiLabel, "burner_warming_up_attended") == 0) {
    return "burner_warming_up_attended";
  }

  // FIX 2: Trust EI for cooling_down more broadly — removed the < 50C restriction
  // EI correctly identifies cooling_down, we should not ignore it above 50C
  if (strcmp(eiLabel, "burner_cooling_down_unattended") == 0 && meaningful_cooling) {
    return "burner_cooling_down_unattended";
  }

  // FIX 3: Temperature floor — prevents burner_idle above 35C
  // Only fires when EI does NOT say cooling_down (to avoid overriding correct cooling detection)
  if (current_mlx >= 35.0f && strcmp(eiLabel, "burner_cooling_down_unattended") != 0) {
    return (current_mlx < HOT_THRESHOLD_C) ? "burner_warming_up_attended" : "burner_hot_unattended";
  }

  return "burner_idle";
}

void runInference() {
  inference_count++;

  signal_t signal;
  signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
  signal.get_data = &raw_feature_get_data;

  ei_impulse_result_t result = { 0 };

  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    Serial.print("Inference failed: ");
    Serial.println((int)err);
    return;
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
  float current_mlx = features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - FEATURES_PER_SAMPLE];
  float mlx_rise = current_mlx - baseline_mlx;

  int motion_count = 0;
  for (size_t i = 2; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i += FEATURES_PER_SAMPLE) {
    if (features[i] > 0.5f) {
      motion_count++;
    }
  }

  const char* finalState = getFinalState(bestLabel, baseline_mlx, current_mlx, motion_count);

  printDivider();
  Serial.print("[INFERENCE ");
  Serial.print(inference_count);
  Serial.println("]");

  Serial.print("Test stage note: ");
  Serial.println(TEST_STAGE);

  Serial.print("EI top class: ");
  Serial.print(bestLabel);
  Serial.print(" | confidence = ");
  Serial.println(bestValue, 5);

  Serial.print("MLX baseline = ");
  Serial.println(baseline_mlx, 2);

  Serial.print("MLX current  = ");
  Serial.println(current_mlx, 2);

  Serial.print("MLX rise     = ");
  Serial.println(mlx_rise, 2);

  Serial.print("Motion count in window = ");
  Serial.println(motion_count);

  Serial.print("FINAL STATE = ");
  Serial.println(finalState);

  Serial.println("All class scores:");
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    Serial.print("  ");
    Serial.print(result.classification[i].label);
    Serial.print(": ");
    Serial.println(result.classification[i].value, 5);
  }

  printDivider();
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  unsigned long start = millis();
  while (!Serial && millis() - start < 8000) {
    delay(10);
  }

  delay(1000);
  Serial.println("setup: serial started");

  pinMode(PIR_PIN, INPUT);
  Serial.println("setup: PIR pin set");

  Wire.setSDA(0);
  Wire.setSCL(1);
  Wire.begin();
  Serial.println("setup: I2C begun on SDA=GP0, SCL=GP1");

  delay(100);

  if (!mlx.begin(0x5A, &Wire)) {
    Serial.println("setup: ERROR MLX90614 not found at 0x5A");
    while (1) delay(100);
  }
  Serial.println("setup: MLX90614 found");

  ds.begin();
  Serial.println("setup: DS18B20 begin done");

  if (ds.getDeviceCount() > 0 && ds.getAddress(dsAddr, 0)) {
    dsFound = true;
    ds.setResolution(dsAddr, 12);
    Serial.println("setup: DS18B20 found");
  } else {
    Serial.println("setup: WARNING DS18B20 not found");
  }

  Serial.print("setup: EI frame size = ");
  Serial.println(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);

  Serial.print("setup: EI label count = ");
  Serial.println(EI_CLASSIFIER_LABEL_COUNT);

  run_classifier_init();
  Serial.println("setup: classifier initialized");

  Serial.println("setup: warming sensors for 30s...");
  delay(30000);

  Serial.println("setup: done");
  last_sample_ms = millis();
}

void loop() {
  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) {
    return;
  }
  last_sample_ms = millis();

  Serial.println("loop: taking sample");

  float sample[FEATURES_PER_SAMPLE];
  if (!readSensorSample(sample)) {
    Serial.println("loop: sensor read failed");
    return;
  }

  sample_count++;
  Serial.print("[SAMPLE ");
  Serial.print(sample_count);
  Serial.println("]");

  Serial.print("loop: raw = ");
  Serial.print(sample[0], 2);
  Serial.print(",");
  Serial.print(sample[1], 2);
  Serial.print(",");
  Serial.println((int)sample[2]);

  for (size_t i = 0; i < FEATURES_PER_SAMPLE; i++) {
    if (feature_ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
      features[feature_ix++] = sample[i];
    }
  }

  Serial.print("loop: feature_ix = ");
  Serial.println(feature_ix);

  if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    Serial.println("loop: running inference");
    runInference();
    feature_ix = 0;
    Serial.println("loop: inference done");
  }
}
