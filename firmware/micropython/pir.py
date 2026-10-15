from machine import Pin
import time

pir = Pin(16, Pin.IN)

print("Warmup 30 seconds...")
time.sleep(30)
print("Ready.")

last = pir.value()

while True:
    current = pir.value()
    if current != last:
        print("Motion!" if current else "No motion")
        last = current
    time.sleep(0.05)