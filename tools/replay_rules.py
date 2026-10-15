#!/usr/bin/env python3
"""Replay the stove decision rules over the recorded dataset, with no hardware.

A direct Python port of `getFinalState()` and the window logic in
firmware/arduino/stove_monitor_final/stove_monitor_final.ino (added when this repo
was prepared). It runs the RULE LAYER ONLY: the Edge Impulse model is not included,
so the model's label is treated as unknown. The shipped system combines the model
with these rules, so these numbers describe the rules alone, not the whole system.

    python tools/replay_rules.py [data_collection/data_logs]
"""
import csv
import glob
import os
import sys
from collections import Counter, defaultdict

SAMPLES_PER_WINDOW = 12        # EI_CLASSIFIER_RAW_SAMPLE_COUNT of the deployed model
HOT_THRESHOLD_C = 65.0
ATTENDED_MOTION_COUNT = 3
WARMUP_RISE_C = 2.0
LABELS = {  # folder name -> state label (the model used "burner_*" names for burner and oven)
    "idle_unattended": "burner_idle",
    "warming_up_attended": "burner_warming_up_attended",
    "hot_attended": "burner_hot_attended",
    "hot_unattended": "burner_hot_unattended",
    "cooling_down_unattended": "burner_cooling_down_unattended",
}


def final_state(ei_label, baseline, current, motion_count):
    """Same order of checks as getFinalState() in the sketch. ei_label may be ''."""
    rise = current - baseline
    started_near_room = 18.0 <= baseline <= 35.0
    warming_trend = rise >= WARMUP_RISE_C
    meaningful_cooling = rise <= -0.5
    started_warm_or_hot = baseline >= 40.0
    still_clearly_hot = current >= HOT_THRESHOLD_C
    hot = "burner_hot_attended" if motion_count >= ATTENDED_MOTION_COUNT else "burner_hot_unattended"

    rapid_rise = rise >= 3.0
    if rapid_rise and current >= HOT_THRESHOLD_C:
        return hot
    if rapid_rise and current >= 35.0:
        return "burner_warming_up_attended"
    if started_warm_or_hot and meaningful_cooling and not still_clearly_hot:
        return "burner_cooling_down_unattended"
    if current >= HOT_THRESHOLD_C:
        return hot
    if 58.0 <= current < HOT_THRESHOLD_C:
        if ei_label == "burner_hot_attended" and motion_count >= ATTENDED_MOTION_COUNT:
            return "burner_hot_attended"
        if ei_label == "burner_hot_unattended" and motion_count < ATTENDED_MOTION_COUNT:
            return "burner_hot_unattended"
    if rise >= 1.5 and motion_count >= 1 and 22.0 <= current < HOT_THRESHOLD_C:
        return "burner_warming_up_attended"
    if started_near_room and warming_trend:
        return "burner_warming_up_attended"
    if ei_label == "burner_warming_up_attended":
        return "burner_warming_up_attended"
    if ei_label == "burner_cooling_down_unattended" and meaningful_cooling:
        return "burner_cooling_down_unattended"
    if current >= 35.0 and ei_label != "burner_cooling_down_unattended":
        return "burner_warming_up_attended" if current < HOT_THRESHOLD_C else "burner_hot_unattended"
    return "burner_idle"


def windows(path):
    """Non-overlapping windows of 12 valid samples: (baseline, current, motion_count)."""
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                mlx, motion = float(r["mlx_temp"]), float(r["motion"])
            except (ValueError, KeyError):
                continue
            rows.append((mlx, motion))
    for i in range(0, len(rows) - SAMPLES_PER_WINDOW + 1, SAMPLES_PER_WINDOW):
        w = rows[i:i + SAMPLES_PER_WINDOW]
        if any(m <= -900 for m, _ in w):       # -999 marks a failed sensor read
            continue
        yield w[0][0], w[-1][0], sum(1 for _, mo in w if mo > 0.5)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "data_collection", "data_logs")
    confusion = defaultdict(Counter)
    for path in sorted(glob.glob(os.path.join(root, "*", "*", "*.csv"))):
        folder = os.path.basename(os.path.dirname(path))
        truth = LABELS.get(folder)
        if not truth:
            continue
        for baseline, current, motion in windows(path):
            confusion[truth][final_state("", baseline, current, motion)] += 1
    names = list(LABELS.values())
    total = sum(sum(c.values()) for c in confusion.values())
    correct = sum(confusion[t][t] for t in names)
    print(f"Rules only (no model), {SAMPLES_PER_WINDOW}-sample windows, {total} windows\n")
    print(f"{'recorded as':34s}" + "".join(f"{n.replace('burner_', '')[:11]:>13s}" for n in names) + "   correct")
    for t in names:
        row = confusion[t]
        n = sum(row.values())
        pct = f"{100 * row[t] / n:5.1f}%" if n else "   n/a"
        print(f"{t.replace('burner_', ''):34s}" + "".join(f"{row[p]:13d}" for p in names) + f"   {pct} of {n}")
    print(f"\nOverall agreement with the recorded label: {100 * correct / total:.1f}%")


if __name__ == "__main__":
    main()
