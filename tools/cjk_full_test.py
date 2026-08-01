# -*- coding: utf-8 -*-
"""Full test: wait boot -> send prompt -> wait done -> wait ready -> SHOOT.
Robust base64 capture (join lines then strip non-base64)."""
import serial, json, time, sys, io, base64, re
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
PROJECT = Path(r'D:\codes\esp32-ai')
outdir = PROJECT / 'output'
outdir.mkdir(exist_ok=True)

prompt = sys.argv[1] if len(sys.argv) > 1 else '本报告'
max_tok = int(sys.argv[2]) if len(sys.argv) > 2 else 60

tok = json.loads((PROJECT / 'data_chinese' / 'tokenizer.json').read_text(encoding='utf-8'))
stoi = tok['stoi']
ids = [stoi.get(ch, stoi.get('<UNK>', 1)) for ch in prompt]
payload = json.dumps({'ids': ids, 'max': max_tok}, separators=(',', ':'))

ser = serial.Serial('COM4', 115200, timeout=10)
ser.reset_input_buffer()
ser.reset_output_buffer()

# wait ready
print('wait ready...', flush=True)
deadline = time.time() + 120
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    if '"ready"' in raw.decode('utf-8', errors='replace'):
        break

# send prompt
ser.write((payload + '\n').encode('utf-8'))
ser.flush()
print(f'sent {prompt!r} ids={ids} max={max_tok}', flush=True)

# wait done
print('generating...', flush=True)
deadline = time.time() + 180
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    line = raw.decode('utf-8', errors='replace')
    if '"done"' in line:
        print('done', flush=True)
        break

# wait ready again (device loops back)
print('wait ready#2...', flush=True)
deadline = time.time() + 15
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    if '"ready"' in raw.decode('utf-8', errors='replace'):
        break
time.sleep(0.3)

# SHOOT
ser.write(b'SHOOT\n')
ser.flush()
print('SHOOT sent', flush=True)

b64 = []
in_shot = False
deadline = time.time() + 30
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    line = raw.decode('utf-8', errors='replace').strip()
    if 'SCREENSHOT_START' in line:
        in_shot = True; continue
    if 'SCREENSHOT_END' in line:
        break
    if in_shot and line:
        b64.append(line)
ser.close()

joined = ''.join(b64)
# strip anything that isn't base64
joined = re.sub(r'[^A-Za-z0-9+/=]', '', joined)
# PBM = 13 header + 15000 px = 15013 bytes -> base64 len 20020
# If length mod 4 == 1, the final chunk lost 3 bytes (line ended mid-way);
# re-append '=' padding by DROPPING the trailing partial quad and padding.
mod = len(joined) % 4
if mod != 0:
    # drop trailing incomplete base64 quad (<=3 chars) then pad
    joined = joined[:len(joined) - mod]
joined += '=' * ((4 - len(joined) % 4) % 4)
print(f'b64 chunks={len(b64)} joined_len={len(joined)}', flush=True)

try:
    pbm = base64.b64decode(joined)
except Exception as e:
    print(f'base64 error: {e}'); sys.exit(1)
ts = time.strftime('%Y%m%d_%H%M%S')
pbm_path = outdir / f'cjk_final_{ts}.pbm'
pbm_path.write_bytes(pbm)
print(f'saved {pbm_path} ({len(pbm)} bytes)', flush=True)
