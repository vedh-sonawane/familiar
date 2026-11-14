import serial, time, sys

PORT = 'COM3'
OUT = r'C:\Users\sonaw\Projects\familiar\wifi_capture.log'
WINDOW = 150  # seconds

try:
    s = serial.Serial(PORT, 115200, timeout=1)
except Exception as e:
    print("OPEN_FAIL", e)
    sys.exit(1)

print("CAPTURING")
sys.stdout.flush()
end = time.time() + WINDOW
with open(OUT, 'wb') as f:
    while time.time() < end:
        try:
            data = s.read(4096)
        except Exception as e:
            f.write(("\n[READ_ERR] %s\n" % e).encode())
            f.flush()
            break
        if data:
            f.write(data)
            f.flush()
try:
    s.close()
except Exception:
    pass
print("DONE")
