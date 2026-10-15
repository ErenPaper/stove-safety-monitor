# Stove Safety Monitor

A stove-safety subsystem for an elder-care monitor, running on a Raspberry Pi Pico 2W. It watches both a **stove burner** and an **oven** and reports when each one starts, how long it was on, and when it has been left running. Burner states depend on whether someone is nearby, so the condition that matters is a hot burner with nobody there.

**Demo:** https://youtu.be/SDkYA16DcgQ
**Stack:** C++ (Arduino) · MicroPython · Edge Impulse (TinyML) · I2C · 1-Wire · MQTT over TLS (AWS IoT Core) · Raspberry Pi Pico 2W

> This is the stove-safety subsystem of the capstone project *Continuing Care Home Activity Monitor* (University of Alberta, Jan. to Apr. 2026). The capstone had three subsystems (stove, water, appliances) plus a cloud pipeline and a caregiver app. **I designed and built this subsystem, including the data collection, the model and the decision logic.** The water and appliance subsystems, the AWS-side processing and the caregiver app were my teammates' work and are not in this repo.

## Sensors

| Value | Sensor | Interface |
|---|---|---|
| Surface (object) temperature | MLX90614 infrared thermometer | I2C, address `0x5A`, SDA `GP0`, SCL `GP1` |
| Contact temperature | DS18B20 | 1-Wire, `GP15` |
| Motion (0 or 1) | PIR sensor | digital input, `GP16` |

Every 1.75 s the firmware reads all three and collects 12 samples (about 20 s) into a window.

## Two appliances, two methods

The recordings show that the two appliances produce different signals (mean readings per class):

| | IR surface temperature | DS18B20 contact temperature |
|---|---|---|
| Burner, hot | 99 to 112 C | about 22 C (room temperature) |
| Oven, hot | about 30 C (max 34 C) | 62 to 72 C |
| Idle, either | about 22 C | about 22 C |

On a burner the infrared sensor sees the heat and the contact probe does not. On the oven, whose surface the infrared sensor sees from outside, it is the other way round. So the two use different methods:

- **Burner:** an **Edge Impulse** classifier (five labels: idle, warming up, hot attended, hot unattended, cooling down) plus a rule layer on the infrared temperature and the motion count. The rules add what the model alone got wrong at the boundaries: a hot threshold of 65 C, a warming rise of 2.0 C (3.0 C counts as a rapid rise), a cooling drop of 0.5 C, and three motion events in the window to call the stove "attended". The comments in the sketch record the fixes after each failed test run.
- **Oven:** **rules only**, driven by the contact probe (`getOvenState`): hot at 62 C, warming above 32 C, cooling until it returns to 24 C. An Edge Impulse project for the oven also exists, but the final sketch does not use it.

Both state machines debounce (a state must hold for two consecutive windows), track a session from start to stop, and publish JSON events to **AWS IoT Core over MQTT with TLS**: a start event, and a stop event with durations and peak temperatures. Events are queued (up to 10) when Wi-Fi is down and sent when it returns, and timestamps come from NTP. The firmware also sends an early stop-style event, and logs an anomaly, when a burner has been hot and unattended for 25 consecutive windows (roughly nine minutes) or an oven has been hot for 3 hours.

In my capstone testing the deployed classifier reached **97.73 % accuracy** with an alert response **under 10 seconds** (see the video).

## Repository layout

```
firmware/
  arduino/stove_monitor_full/    the integrated sketch: burner + oven, debounce, MQTT events
    secrets.example.h            copy to secrets.h and fill in (secrets.h is git-ignored)
  arduino/stove_monitor_final/   the earlier standalone burner sketch: model + rules, serial output only
  micropython/                   small scripts used to bring up and log each sensor
data_collection/
  logger.py                      records one 60 s labelled session from the Pico over USB serial
  data_logs/{burner,oven}/<state>_<attendance>/*.csv   152 labelled recordings
tools/
  replay_rules.py                runs both rule sets over the dataset, no hardware needed
```

### Dataset

Each CSV is one 60-second recording with the columns `mlx_temp,ds_temp,motion`. The folder name is the label. Burner has 71 recordings and oven has 81, across idle, warming up, hot (attended and unattended) and cooling down. Record more with:

```sh
pip install -r data_collection/requirements.txt
python data_collection/logger.py burner hot attended    # device, state, attendance
```
Set `PORT` at the top of `logger.py` for your machine.

## Replaying the rules without hardware

`tools/replay_rules.py` ports both rule sets to Python and runs them over the dataset in the same 12-sample windows the firmware uses. For the burner it runs the rules **without the model's label** (the model is not included), so it shows what the rules do alone.

```sh
python tools/replay_rules.py
```

| Recorded as | Rules agree |
|---|---|
| burner idle | 100 % (16 of 16) |
| burner warming up | 100 % (8 of 8) |
| burner hot attended | 87.5 % (14 of 16) |
| burner hot unattended | 100 % (15 of 15) |
| burner cooling down | 18.8 % (3 of 16) |
| oven idle | 100 % (17 of 17) |
| oven hot | 100 % (74 of 74) |
| oven warming up | 41.7 % (10 of 24) |
| oven cooling down | 0 % (0 of 33) |

Read the weak rows as properties of a short recording, not just of the rules. The oven's cooling recordings stay above 62 C for their whole 60 s, and the rules only call "cooling" once the probe has fallen below that after being hot, so a one-minute slice looks like "hot". Warming recordings start near room temperature and the oven heats slowly, so early windows look idle. In real use the state machine carries its history across windows, which a single recording cannot. For the burner, the model is meant to cover cooling down. This replay is not a measurement of the deployed system.

## Running it

1. Wire the sensors as in the table above and flash the MicroPython scripts in `firmware/micropython/` to check each sensor.
2. Copy `firmware/arduino/stove_monitor_full/secrets.example.h` to `secrets.h` and fill in your Wi-Fi and AWS IoT values. Never commit `secrets.h`.
3. Train your own burner model in [Edge Impulse](https://edgeimpulse.com) from the burner CSVs (three inputs, one 12-sample window) and export it as an **Arduino library**. The sketch includes `<erenpaper-project-1_inferencing.h>`, the library exported from my own project. It is generated code tied to that project, so it is not included. Change that include to your library's name.
4. Build with the Arduino IDE and the Raspberry Pi Pico (arduino-pico) board package, with the Adafruit MLX90614, OneWire, DallasTemperature and PubSubClient libraries installed.

## Limitations

- A prototype tuned on one stove and one oven. The thresholds and the model will need retuning for different appliances, sensor placement or rooms.
- Not a safety device. It reports events; what happens to them in the cloud and on a caregiver's phone was another team member's part of the capstone.
- I did not recompile the sketches when preparing this repo, because they need the exported Edge Impulse library and your own credentials.
- The credentials that were in my working copy of the sketch (Wi-Fi, AWS endpoint and device keys) were removed before publishing.

## License
MIT. See [LICENSE](LICENSE).
