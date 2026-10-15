import onewire, ds18x20, machine, time

dat = machine.Pin(15)  # Change if you used a different GPIO
ds = ds18x20.DS18X20(onewire.OneWire(dat))

roms = ds.scan()
print("Found devices:", roms)

while True:
    ds.convert_temp()
    time.sleep_ms(750)
    for rom in roms:
        print("DS Temp:", ds.read_temp(rom))
    time.sleep(1)