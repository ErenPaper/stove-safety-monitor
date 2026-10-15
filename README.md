# Stove Safety Monitor

A stove-safety subsystem for an elder-care monitor, running on a Raspberry Pi Pico 2W. It watches a stove burner or an oven with a non-contact infrared sensor, adds a contact temperature probe and a motion sensor, and decides whether the appliance is idle, warming up, hot with someone nearby, hot with nobody nearby, or cooling down. Hot with nobody nearby is the condition that matters.

**Demo:** https://youtu.be/SDkYA16DcgQ
**Stack:** C++ (Arduino) · MicroPython · Edge Impulse (TinyML) · I2C · 1-Wire · Raspberry Pi Pico 2W

> This is the stove-safety subsystem of the capstone project *Continuing Care Home Activity Monitor* (University of Alberta, Jan. to Apr. 2026). The capstone had three subsystems (stove, water, appliances) plus a cloud pipeline and a caregiver app. **I designed and built this subsystem, including the data collection, the model and the decision logic.** The other parts were my teammates' work and are not in this repo.

## How it works

Every 1.75 s the firmware reads three values and appends them to a frame:

| Value | Sensor | Interface |
|---|---|---|
| Surface (object) temperature | MLX90614 infrared thermometer | I2C, address `0x5A`, SDA `GP0`, SCL `GP1` |
| Contact temperature | DS18B20 | 1-Wire, `GP15` |
| Motion (0 or 1) | PIR sensor | digital input, `GP16` |

When a frame is full it is classified by an **Edge Impulse** model trained on the recordings in `data_collection/`. A rule layer (`getFinalState` in the sketch) then combines the model's label with simple physical checks and returns one of five states:

`burner_idle` · `burner_warming_up_attended` · `burner_hot_attended` · `burner_hot_unattended` · `burner_cooling_down_unattended`

The rules exist because the model alone was unreliable at the boundaries. They use a hot threshold of 65 C, a warming rise of 2.0 C (3.0 C counts as a rapid rise), a cooling drop of 0.5 C, and a count of three motion events in the window to call the stove "attended". The comments in the sketch record the fixes I made after each failed test run.

In my capstone testing the deployed classifier reached **97.73 % accuracy** with an alert response **under 10 seconds** (see the video).

## Repository layout

```
firmware/
  arduino/stove_monitor_final/   the Pico 2W sketch: sensing, inference, decision rules
  micropython/                   small scripts used to bring up and log each sensor
data_collection/
  logger.py                      records one 60 s labelled session from the Pico over USB serial
  data_logs/{burner,oven}/<state>_<attendance>/*.csv   152 labelled recordings
```

### Dataset

Each CSV is one 60-second recording with the columns `mlx_temp,ds_temp,motion`. The folder name is the label. Burner has 71 recordings and oven has 81, across idle, warming up (attended), hot (attended and unattended) and cooling down (unattended). Record more with:

```sh
pip install pyserial
python data_collection/logger.py burner hot attended    # device, state, attendance
```
Set `PORT` at the top of `logger.py` for your machine.

## Running it

1. Wire the sensors as in the table above and flash the MicroPython scripts in `firmware/micropython/` to check each sensor.
2. Train your own model in [Edge Impulse](https://edgeimpulse.com) from the CSVs (three inputs, one 60 s window) and export it as an **Arduino library**.
3. The sketch includes `<erenpaper-project-1_inferencing.h>`, the library exported from my own Edge Impulse project. It is generated code tied to that project, so it is not included. Change that include to the name of your exported library.
4. Build `stove_monitor_final.ino` with the Arduino IDE and the Raspberry Pi Pico (arduino-pico) board package, with the Adafruit MLX90614, OneWire and DallasTemperature libraries installed.

## Limitations

- A prototype tuned on one stove and one oven. The thresholds and the model will need retuning for a different appliance, sensor position or room.
- Not a safety device. It reports a state over serial; the alerting path to a caregiver was another team member's part of the capstone.
- I did not compile the sketch again when preparing this repo, because it needs the exported Edge Impulse library.

## License
MIT. See [LICENSE](LICENSE).
