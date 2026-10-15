import os
import sys
import csv
import time
from datetime import datetime
import serial

PORT = "/dev/tty.usbmodem1101"   # change if needed "/dev/ttyACM0"
BAUD = 115200

duration = 60  # seconds to record


if len(sys.argv) != 4:
    print("Usage: python logger.py <device> <state> <attendance>")
    sys.exit()

device = sys.argv[1]      # burner / oven
state = sys.argv[2]       # idle / warmup / etc
attendance = sys.argv[3]  # attended/unattended


folder = f"data_logs/{device}/{state}_{attendance}"
os.makedirs(folder, exist_ok=True)


timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
filename = f"{folder}/{state}_{timestamp}.csv"


ser = serial.Serial(PORT, BAUD, timeout=1)

print(f"Recording {state} for {duration} seconds...")
print(f"Saving to {filename}")

with open(filename, "w", newline="") as f:
    writer = csv.writer(f)

    writer.writerow(["mlx_temp","ds_temp","motion"])

    start = time.time()

    while time.time() - start < duration:
        line = ser.readline().decode().strip()

        if line:
            values = line.split(",")
            if len(values) == 3:
                writer.writerow(values)

print("Done.")