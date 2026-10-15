from machine import Pin, I2C
import onewire, ds18x20
import time

# ---------- PIR ----------
pir = Pin(16, Pin.IN)

# ---------- DS18B20 ----------
ds_pin = Pin(15)
ds = ds18x20.DS18X20(onewire.OneWire(ds_pin))
roms = ds.scan()

if not roms:
    print("ERROR_DS18_NOT_FOUND")

# ---------- MLX90614 ----------
i2c = I2C(0, sda=Pin(0), scl=Pin(1), freq=100000)

def read_mlx():
    try:
        data = i2c.readfrom_mem(0x5A, 0x07, 3)
        temp = (data[1] << 8 | data[0]) * 0.02 - 273.15
        return temp
    except:
        return None

print("mlx_temp,ds_temp,motion")

# ---------- Sensor warmup ----------
print("Warming sensors for 30s...")
time.sleep(30)

# ---------- Main loop ----------
while True:

    # --- DS18 temperature ---
    ds_temp = None
    if roms:
        try:
            ds.convert_temp()
            time.sleep_ms(750)
            ds_temp = ds.read_temp(roms[0])
        except:
            ds_temp = None

    # --- MLX temperature ---
    mlx_temp = read_mlx()

    # --- PIR motion ---
    motion = pir.value()

    # --- Replace None values ---
    if mlx_temp is None:
        mlx_temp = -999

    if ds_temp is None:
        ds_temp = -999

    # --- Output CSV line ---
    print("{:.2f},{:.2f},{}".format(mlx_temp, ds_temp, motion))

    # ~1.75s sample rate
    time.sleep(1)