# -*- coding: utf-8 -*-
"""Mid-generation capture: send prompt, wait a few seconds (mid-gen), capture SHOOT,
then compare with post-gen capture. Goal: verify cursor behavior during generation."""
import serial, json, time, sys, io, base64, re
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
PROJECT = Path(r'D:\codes\esp32-ai')
outdir = PROJECT / 'output'
outdir.mkdir(exist_ok=True)

tok = json.loads((PROJECT / 'data_chinese' / 'tokenizer.json').read_text(encoding='utf-8'))
stoi = tok['stoi']
ids = [stoi.get(ch, 1) for ch in '本报告']
payload = json.dumps({'ids': ids, 'max': 200}, separators=(',', ':'))

ser = serial.Serial('COM4', 115200, timeout=10)
ser.reset_input_buffer()
deadline = time.time() + 60
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    if '"ready"' in raw.decode('utf-8', errors='replace'):
        break

ser.write((payload + '\n').encode('utf-8'))
ser.flush()
print('prompt sent, waiting 8s (mid-generation)...', flush=True)
time.sleep(8)

def shoot(tag):
    ser.write(b'SHOOT\n'); ser.flush()
    b64 = []; in_shot = False
    deadline = time.time() + 20
    while time.time() < deadline:
        raw = ser.readline()
        if not raw: continue
        line = raw.decode('utf-8', errors='replace').strip()
        if 'SCREENSHOT_START' in line: in_shot = True; continue
        if 'SCREENSHOT_END' in line: break
        if in_shot and line: b64.append(line)
    joined = re.sub(r'[^A-Za-z0-9+/=]', '', ''.join(b64))
    mod = len(joined) % 4
    if mod != 0: joined = joined[:len(joined) - mod]
    joined += '=' * ((4 - len(joined) % 4) % 4)
    try:
        pbm = base64.b64decode(joined)
        ts = time.strftime('%H%M%S')
        p = outdir / f'cjk_{tag}_{ts}.pbm'
        p.write_bytes(pbm)
        print(f'{tag}: saved {p.name} ({len(pbm)} B)')
        return p
    except Exception as e:
        print(f'{tag}: decode err {e} joined_len={len(joined)}')
        return None

mid = shoot('mid')
# wait for done
print('waiting for done...', flush=True)
deadline = time.time() + 120
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    if '"done"' in raw.decode('utf-8', errors='replace'):
        break
time.sleep(0.5)
# wait ready then shoot post
deadline = time.time() + 15
while time.time() < deadline:
    raw = ser.readline()
    if not raw: continue
    if '"ready"' in raw.decode('utf-8', errors='replace'):
        break
time.sleep(0.3)
post = shoot('post')
ser.close()
print('done')
