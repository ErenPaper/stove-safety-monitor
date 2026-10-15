#!/usr/bin/env python3
"""Replay the stove decision rules over the recorded dataset, with no hardware.

Python ports of the rule code in firmware/arduino/stove_monitor_full/ (added when this
repo was prepared):
  * burner: getBurnerState-style rule layer from stove_monitor_final.ino, run WITHOUT the
    Edge Impulse label (the model is not included), so it shows what the rules alone do;
  * oven:   getOvenState(), which is rules-only on the DS18B20 contact probe.
Each recording is cut into the same non-overlapping 12-sample windows the firmware uses
(about 20 s). Debouncing and the MQTT alerts are not replayed.

    python tools/replay_rules.py [data_collection/data_logs]
"""
import csv
import glob
import os
import sys
from collections import Counter, defaultdict

SAMPLES_PER_WINDOW = 12        # EI_CLASSIFIER_RAW_SAMPLE_COUNT of the deployed model

# ---- burner (same checks, same order, as getFinalState() in stove_monitor_final.ino)
BURNER_HOT_C, ATTENDED_MOTION, WARMUP_RISE_C = 65.0, 3, 2.0
BURNER_LABELS = {
    "idle_unattended": "burner_idle",
    "warming_up_attended": "burner_warming_up_attended",
    "hot_attended": "burner_hot_attended",
    "hot_unattended": "burner_hot_unattended",
    "cooling_down_unattended": "burner_cooling_down_unattended",
}


def burner_state(ei_label, baseline, current, motion_count):
    rise = current - baseline
    hot = "burner_hot_attended" if motion_count >= ATTENDED_MOTION else "burner_hot_unattended"
    rapid_rise = rise >= 3.0
    if rapid_rise and current >= BURNER_HOT_C:
        return hot
    if rapid_rise and current >= 35.0:
        return "burner_warming_up_attended"
    if baseline >= 40.0 and rise <= -0.5 and current < BURNER_HOT_C:
        return "burner_cooling_down_unattended"
    if current >= BURNER_HOT_C:
        return hot
    if 58.0 <= current < BURNER_HOT_C:
        if ei_label == "burner_hot_attended" and motion_count >= ATTENDED_MOTION:
            return "burner_hot_attended"
        if ei_label == "burner_hot_unattended" and motion_count < ATTENDED_MOTION:
            return "burner_hot_unattended"
    if rise >= 1.5 and motion_count >= 1 and 22.0 <= current < BURNER_HOT_C:
        return "burner_warming_up_attended"
    if 18.0 <= baseline <= 35.0 and rise >= WARMUP_RISE_C:
        return "burner_warming_up_attended"
    if ei_label == "burner_warming_up_attended":
        return "burner_warming_up_attended"
    if ei_label == "burner_cooling_down_unattended" and rise <= -0.5:
        return "burner_cooling_down_unattended"
    if current >= 35.0 and ei_label != "burner_cooling_down_unattended":
        return "burner_warming_up_attended" if current < BURNER_HOT_C else "burner_hot_unattended"
    return "burner_idle"


# ---- oven (getOvenState() in stove_monitor_full.ino; it keeps state between windows)
OVEN_HOT_C, OVEN_WARM_C, OVEN_ROOM_C = 62.0, 32.0, 24.0
OVEN_LABELS = {
    "idle_unattended": "oven_idle",
    "warming_up_attended": "oven_warming_up",
    "hot_attended": "oven_hot",          # the oven model has no attended/unattended split
    "hot_unattended": "oven_hot",
    "cooling_down_unattended": "oven_cooling_down",
}


class OvenRules:
    def __init__(self):
        self.peak = 0.0
        self.in_cooling = False

    def state(self, current_ds, ds_rise):
        self.peak = max(self.peak, current_ds)
        if not self.in_cooling and self.peak >= OVEN_HOT_C and current_ds < OVEN_HOT_C:
            self.in_cooling = True
        if self.in_cooling and current_ds <= OVEN_ROOM_C:
            self.in_cooling = False
            self.peak = current_ds
        if current_ds >= OVEN_HOT_C:
            return "oven_hot"
        if self.in_cooling and ds_rise <= -0.05:
            return "oven_cooling_down"
        if self.in_cooling and current_ds > OVEN_ROOM_C:
            return "oven_cooling_down"
        if current_ds >= OVEN_WARM_C and ds_rise > 0.02:
            return "oven_warming_up"
        if OVEN_WARM_C <= current_ds < OVEN_HOT_C and self.peak >= OVEN_WARM_C:
            return "oven_warming_up"
        return "oven_idle"


def windows(path):
    """Non-overlapping windows of valid samples: lists of (mlx, ds, motion)."""
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((float(r["mlx_temp"]), float(r["ds_temp"]), float(r["motion"])))
            except (ValueError, KeyError):
                continue
    for i in range(0, len(rows) - SAMPLES_PER_WINDOW + 1, SAMPLES_PER_WINDOW):
        w = rows[i:i + SAMPLES_PER_WINDOW]
        if any(m <= -900 or d <= -900 for m, d, _ in w):    # -999 marks a failed sensor read
            continue
        yield w


def replay(root, device):
    confusion = defaultdict(Counter)
    labels = BURNER_LABELS if device == "burner" else OVEN_LABELS
    for path in sorted(glob.glob(os.path.join(root, device, "*", "*.csv"))):
        truth = labels.get(os.path.basename(os.path.dirname(path)))
        oven = OvenRules()                       # fresh state for each recording
        for w in windows(path):
            if device == "burner":
                got = burner_state("", w[0][0], w[-1][0], sum(1 for *_, mo in w if mo > 0.5))
            else:
                got = oven.state(w[-1][1], w[-1][1] - w[0][1])
            confusion[truth][got] += 1
    return confusion, list(dict.fromkeys(labels.values()))


def report(device, confusion, names):
    total = sum(sum(c.values()) for c in confusion.values())
    correct = sum(confusion[t][t] for t in names)
    short = lambda n: n.split("_", 1)[1][:12]
    print(f"\n{device.upper()}  ({total} windows, rules only)")
    print(f"{'recorded as':32s}" + "".join(f"{short(n):>14s}" for n in names) + "   rules agree")
    for t in names:
        row, n = confusion[t], sum(confusion[t].values())
        print(f"{t:32s}" + "".join(f"{row[p]:14d}" for p in names) + (f"   {100 * row[t] / n:5.1f}% of {n}" if n else "   n/a"))
    print(f"overall: {100 * correct / total:.1f}%")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "data_collection", "data_logs")
    for device in ("burner", "oven"):
        report(device, *replay(root, device))


if __name__ == "__main__":
    main()
