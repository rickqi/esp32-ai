# -*- coding: utf-8 -*-
"""Shoot-only: send SHOOT and save screenshot to output/."""
import serial, time, sys, io, base64
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
PORT = 'COM4'
outdir = Path(r'D:\codes\esp32-ai\output')
outdir.mkdir(exist_ok=True)

ser = serial.Serial(PORT, 115200, timeout=10)
ser.reset_input_buffer()
ser.reset_output_buffer()

# wait for ready if device is mid-generation
print('waiting for ready...', flush=True)
deadline = time.time() + 120
ready = False
while time.time() < deadline:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode('utf-8', errors='replace')
    ls = line.strip()
    if '"ready"' in ls:
        print('  ready', flush=True)
        ready = True
        break

ser.write(b'SHOOT\n')
ser.flush()
print('SHOOT sent', flush=True)

b64 = []
in_shot = False
deadline = time.time() + 30
while time.time() < deadline:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode('utf-8', errors='replace').strip()
    if 'SCREENSHOT_START' in line:
        in_shot = True; continue
    if 'SCREENSHOT_END' in line:
        break
    if in_shot and len(line) > 5:
        b64.append(line)
ser.close()

if not b64:
    print('ERROR: no screenshot'); sys.exit(1)
pbm = base64.b64decode(''.join(b64))
ts = time.strftime('%Y%m%d_%H%M%S')
pbm_path = outdir / f'cjk_shot_{ts}.pbm'
pbm_path.write_bytes(pbm)
print(f'saved {pbm_path} ({len(pbm)} bytes, 400x300 expected = {400*300//8} B)')
