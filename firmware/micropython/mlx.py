from machine import Pin, I2C
import time

i2c = I2C(0, sda=Pin(0), scl=Pin(1), freq=100000)

def read_mlx():
    data = i2c.readfrom_mem(0x5A, 0x07, 3)
    temp = (data[1] << 8 | data[0]) * 0.02 - 273.15
    return temp

while True:
    print("MLX Temp:", read_mlx())
    time.sleep(1)